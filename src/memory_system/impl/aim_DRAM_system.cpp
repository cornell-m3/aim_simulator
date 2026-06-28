#include "addr_mapper/addr_mapper.h"
#include "base/request.h"
#include "base/type.h"
#include "dram/dram.h"
#include "dram_controller/controller.h"
#include "memory_system/memory_system.h"
#include "translation/translation.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <math.h>
#include <vector>

namespace Ramulator {

#define ISR_SIZE (1 << 21)
// Channel mask is encoded in the lower 32 bits of an int64_t throughout the
// trace format and the dispatch path. Bits 32+ are reserved.
static constexpr int kChannelMaskWidth = 32;
// Synthetic row address used to route ISR_AF aim_reqs. ISR_AF doesn't have a
// real row in the trace; this base + AFM selects the activation-function LUT row.
static constexpr int kAFRowBase = 1 << 29;

// Design D: design B's per-channel pipeline + design C's required-channels
// FIFO join. The cleanest of the four implementations on both axes:
//
//   - send path:  one (drain is the only place that calls controller->send)
//   - join model: per-host required_channels mask + per-channel high-water
//                 mark (last_completed_host_id), so multiple in-flight host
//                 callbacks at different host_req_ids are handled naturally
//
// tick() has three phases:
//   1. decompose: pop one host_req from host_request_queue and split it into
//      per-channel aim_reqs. aim_reqs go straight into channel_pending_queues
//      — decompose never calls controller->send().
//   2. drain: for each channel, send aim_reqs from channel_pending_queues[ch]
//      to the controller until stalls[ch] becomes nonzero (blocking aim_req in
//      flight) or the controller refuses. This is the ONLY send path.
//   3. controllers tick.
//
// Host callbacks (e.g. ISR_EOC's frontend "trace done" hook) live in a FIFO of
// PendingCallback{host_req, required_channels_mask}, pushed at decompose time.
// Each blocking aim_req carries this->callback, so completions land in
// receive(), which (a) decrements stalls[ch] and (b) advances
// last_completed_host_id[ch] to the completing host_req's id. The FIFO is then
// walked head-first: a callback fires once every channel in its required_channels
// mask has caught up (last_completed_host_id[ch] >= pc.host_req.host_req_id).
//
// Race-free by construction: last_completed_host_id only advances from real
// completions, so an unsent aim_req can never trip the barrier — no
// empty-queue clause is needed (unlike design B's all-free barrier).
class AiMDRAMSystem final : public IMemorySystem, public Implementation {
    RAMULATOR_REGISTER_IMPLEMENTATION(IMemorySystem, AiMDRAMSystem, "AiMDRAM", "AiM memory system (AiM DMA).");

protected:
    Clk_t m_clk = 0;
    IDRAM *m_dram;
    int m_num_levels = -1;
    bool m_has_rank = false;
    IAddrMapper *m_addr_mapper;
    std::vector<IDRAMController *> m_controllers;
    Logger_t m_logger;

    // Host requests as they arrive from the frontend; one is decomposed per tick.
    std::queue<Request> host_request_queue;
    // Per-channel FIFO of aim_reqs awaiting controller send. Sized to the actual
    // number of DRAM channels in init().
    std::vector<std::queue<Request>> channel_pending_queues;
    int AiM_req_id = 0;

    // stalls[ch] = number of in-flight blocking aim_reqs on channel ch.
    // A channel with stalls[ch] > 0 won't accept new sends until receive() fires.
    std::vector<int> stalls;
    // High-water mark per channel: largest host_req_id whose blocking aim_req
    // has completed on this channel. Advances only on receive(), so it can't
    // race ahead of actual completions.
    std::vector<int> last_completed_host_id;

    // FIFO of host_reqs whose callback should fire once their required channels
    // (bitmask) have all caught up to host_req_id. Order matches dispatch order.
    struct PendingCallback {
        Request host_req;
        int64_t required_channels;
    };
    std::queue<PendingCallback> pending_callbacks;

    // Bound to receive(); used as the aim_req-completion callback by the controller.
    std::function<void(Request &)> callback;

    enum class CFR {
        BROADCAST,
        EWMUL_BG,
        AFM
    };
    std::map<CFR, int32_t> CFR_values;
    std::map<Addr_t, CFR> address_to_CFR;

    uint8_t CountSetBit(const int64_t ch_mask) const {
        assert(ch_mask > 0);

        uint8_t count = 0;

        for (int i = 0; i < kChannelMaskWidth; i++)
            if (ch_mask & (1LL << i))
                count++;

        return count;
    }

    // Pops the lowest set bit of ch_mask and returns its index in [0, 31].
    // Caller must ensure at least one bit is set.
    uint8_t FindFirstChannelIndex(int64_t &ch_mask) const {
        uint64_t ch_mask_u = static_cast<uint64_t>(ch_mask);
        assert((ch_mask_u & 0xffffffffULL) != 0);

        for (int i = 0; i < kChannelMaskWidth; i++) {
            if (ch_mask_u & (1ULL << i)) {
                ch_mask_u &= ~(1ULL << i);
                ch_mask = static_cast<int64_t>(ch_mask_u);
                return i;
            }
        }

        assert(false);
        return 0;
    }

    void apply_addr_mapp(Request &req, int channel_id) {
        req.addr_vec.resize(m_num_levels, -1);
        if (channel_id < 0 || channel_id >= (int)m_controllers.size()) {
            throw ConfigurationError("AiMDRAMSystem: {} targets channel {} but only {} channels exist!",
                                     req.str(), channel_id, m_controllers.size());
        }
        req.addr_vec[m_dram->m_levels("channel")] = channel_id;
        if (m_has_rank) {
            req.addr_vec[m_dram->m_levels("rank")] = 0;
        }
        if (req.bank_index == -1) {
            req.addr_vec[m_dram->m_levels("bankgroup")] = -1;
            req.addr_vec[m_dram->m_levels("bank")] = -1;
        } else {
            const int banks_per_group = m_dram->get_level_size("bank");
            const int total_banks =
                m_dram->get_level_size("bankgroup") * banks_per_group;
            if (req.bank_index < 0 || req.bank_index >= total_banks) {
                throw ConfigurationError("AiMDRAMSystem: {} bank index {} out of range [0, {})!",
                                         req.str(), req.bank_index, total_banks);
            }
            req.addr_vec[m_dram->m_levels("bankgroup")] = req.bank_index / banks_per_group;
            req.addr_vec[m_dram->m_levels("bank")] = req.bank_index % banks_per_group;
        }
        req.addr_vec[m_dram->m_levels("row")] = req.row_addr;
        req.addr_vec[m_dram->m_levels("column")] = req.col_addr;
    }

public:
    std::map<Type, std::map<MemAccessRegion, int>> s_num_RW_requests;
    std::map<Opcode, int> s_num_AiM_requests;
    int s_ISR_queue_full = 0;
    int s_wait_RD_stall = 0;

public:
    void init() override {
        // Create device (a top-level node wrapping all channel nodes)
        m_dram = create_child_ifce<IDRAM>();
        m_num_levels = m_dram->m_levels.size();
        m_addr_mapper = create_child_ifce<IAddrMapper>();

        m_logger = Logging::create_logger("AiMDRAMSystem");

        if (m_dram->m_levels("bankgroup") - m_dram->m_levels("channel") == 1) {
            m_has_rank = false;
            m_logger->info("AiMDRAMSystem: No rank level in the DRAM system!");
        } else if (m_dram->m_levels("bankgroup") - m_dram->m_levels("channel") == 2) {
            m_has_rank = true;
            m_logger->info("AiMDRAMSystem: Rank level in the DRAM system!");
        } else {
            throw ConfigurationError("AiMDRAMSystem: Invalid number of levels in DRAM {}!", m_dram->get_name());
        }

        int num_channels = m_dram->get_level_size("channel");
        if (num_channels > kChannelMaskWidth) {
            throw ConfigurationError("AiMDRAMSystem: num_channels {} exceeds 32-bit channel mask width!", num_channels);
        }

        // Create memory controllers
        channel_pending_queues.resize(num_channels);
        stalls.assign(num_channels, 0);
        last_completed_host_id.assign(num_channels, -1);
        for (int i = 0; i < num_channels; i++) {
            IDRAMController *controller = create_child_ifce<IDRAMController>();
            controller->m_impl->set_id(fmt::format("Channel {}", i));
            controller->m_channel_id = i;
            m_controllers.push_back(controller);
        }

        m_clock_ratio = param<uint>("clock_ratio").required();

        // vector data for MAC is from GB (0) or next bank (1)
        address_to_CFR[0] = CFR::BROADCAST;
        CFR_values[CFR::BROADCAST] = 0;

        // EWMUL in one bank group (0) or all bank groups (1)
        address_to_CFR[1] = CFR::EWMUL_BG;
        CFR_values[CFR::EWMUL_BG] = 1;

        // Activation Function mode selects AF (0-7)
        address_to_CFR[2] = CFR::AFM;
        CFR_values[CFR::AFM] = 0;

        callback = std::bind(&AiMDRAMSystem::receive, this, std::placeholders::_1);

        register_stat(m_clk).name("memory_system_cycles");
        register_stat(s_wait_RD_stall)
            .name("total_num_wait_read_stalls")
            .desc("total number of cycles that AiM DMA is stalled because of waiting for read operations from channels");

        register_stat(s_ISR_queue_full)
            .name("total_num_ISR_full")
            .desc("total number of cycles that AiM DMA does not receive ISR because of lack of enough ISR space");

        for (const auto type : {Type::Read, Type::Write}) {
            for (const auto mem_access_region : {MemAccessRegion::GPR, MemAccessRegion::CFR, MemAccessRegion::MEM}) {
                s_num_RW_requests[type][mem_access_region] = 0;
                register_stat(s_num_RW_requests[type][mem_access_region])
                    .name(fmt::format("total_num_{}_{}_requests",
                                      AiMISRInfo::convert_type_to_str(type),
                                      AiMISRInfo::convert_mem_access_region_to_str(mem_access_region)))
                    .desc(fmt::format("total number of {} {} requests",
                                      AiMISRInfo::convert_type_to_str(type),
                                      AiMISRInfo::convert_mem_access_region_to_str(mem_access_region)));
            }
        }
        for (int opcode = (int)Opcode::MIN + 1; opcode < (int)Opcode::MAX; opcode++) {
            s_num_AiM_requests[(Opcode)opcode] = 0;
            register_stat(s_num_AiM_requests[(Opcode)opcode])
                .name(fmt::format("total_num_AiM_{}_requests", AiMISRInfo::convert_AiM_opcode_to_str((Opcode)opcode)))
                .desc(fmt::format("total number of AiM {} requests", AiMISRInfo::convert_AiM_opcode_to_str((Opcode)opcode)));
        }
    };

    void setup(IFrontEnd *frontend, IMemorySystem *memory_system) override {}

    bool send(Request req) override {

        if (host_request_queue.size() == ISR_SIZE) {
            s_ISR_queue_full++;
            return false;
        }
        host_request_queue.push(req);

        switch (req.type) {
        case Type::AIM: {
            s_num_AiM_requests[req.opcode]++;
            break;
        }
        case Type::Read:
        case Type::Write: {
            s_num_RW_requests[req.type][req.mem_access_region]++;
            break;
        }
        default: {
            throw ConfigurationError("AiMDRAMSystem: unknown request type {}!", (int)req.type);
            break;
        }
        }

        return true;
    };

    // True if this aim_req carries DMA-blocking semantics (RD_MAC, RD_AF, SYNC,
    // EOC). Used by the drain loop to decide whether to bump stalls[ch] on send.
    // Note: aim_reqs synthesized from ISR_RD_SBK/ISR_WR_SBK are remapped to
    // Type::Read/Write with opcode=Opcode::MIN — those are never blocking.
    bool is_blocking_aim_req(const Request &aim) const {
        if (aim.type != Type::AIM) return false;
        if (aim.opcode == Opcode::MIN) return false;
        return AiMISRInfo::convert_AiM_opcode_to_AiM_ISR(aim.opcode).AiM_DMA_blocking;
    }

    // Split a host_req into per-channel aim_reqs and push each onto the
    // appropriate channel_pending_queues[ch]. Never calls controller->send().
    // If the host_req has a callback (the frontend sets this on ISR_EOC), a
    // PendingCallback is enqueued with required_channels = host_req.channel_mask
    // (or all channels for ISR_SYNC / ISR_EOC) so the barrier in receive() can
    // fire it once every required channel has completed up to this host_req_id.
    void decompose(Request host_req) {
        switch (host_req.type) {
        case Type::AIM: {
            Opcode opcode = host_req.opcode;
            auto opsize = host_req.opsize;
            int64_t ch_mask = host_req.channel_mask;
            uint8_t channel_count = CountSetBit(ch_mask);
            Request aim_req = host_req;
            aim_req.callback = std::function<void(Request &)>();
            if (aim_req.opcode == Opcode::ISR_RD_SBK) {
                aim_req.type = Type::Read;
                aim_req.mem_access_region = MemAccessRegion::MEM;
                aim_req.opcode = Opcode::MIN;
            } else if (aim_req.opcode == Opcode::ISR_WR_SBK) {
                aim_req.type = Type::Write;
                aim_req.mem_access_region = MemAccessRegion::MEM;
                aim_req.opcode = Opcode::MIN;
            }

            switch (opcode) {
            case Opcode::ISR_WR_SBK:
            case Opcode::ISR_WR_GB:
            case Opcode::ISR_WR_BIAS:
            case Opcode::ISR_RD_MAC:
            case Opcode::ISR_RD_AF:
            case Opcode::ISR_RD_SBK:
            case Opcode::ISR_COPY_BKGB:
            case Opcode::ISR_COPY_GBBK:
            case Opcode::ISR_MAC_SBK:
            case Opcode::ISR_MAC_ABK:
            case Opcode::ISR_AF:
            case Opcode::ISR_EWMUL:
            case Opcode::ISR_WR_ABK: {
                AiMISR aim_ISR = AiMISRInfo::convert_AiM_opcode_to_AiM_ISR(opcode);

                if (aim_ISR.AiM_DMA_blocking) {
                    aim_req.callback = callback;
                    if (host_req.callback)
                        pending_callbacks.push({host_req, ch_mask});
                }

                if (aim_ISR.channel_count_eq_one && channel_count != 1) {
                    throw ConfigurationError("AiMDRAMSystem: channel mask ({}) of ISR_WR_SBK must specify only 1 channel!", ch_mask);
                }

                if (opcode == Opcode::ISR_AF) {
                    aim_req.afm = CFR_values[CFR::AFM];
                    aim_req.row_addr = kAFRowBase + aim_req.afm;
                }

                if (host_req.opcode == Opcode::ISR_MAC_ABK || host_req.opcode == Opcode::ISR_MAC_SBK)
                    aim_req.broadcast = CFR_values[CFR::BROADCAST];

                if (host_req.opcode == Opcode::ISR_MAC_ABK)
                    aim_req.ewmul_bg = CFR_values[CFR::EWMUL_BG];

                if (aim_ISR.is_field_legal(AiMISR::Field::bank_index))
                    aim_req.bank_index = host_req.bank_index;

                if (aim_ISR.is_field_legal(AiMISR::Field::row_addr))
                    aim_req.row_addr = host_req.row_addr;

                if (opsize == -1) opsize = 0;
                if (host_req.col_addr == -1) host_req.col_addr = 0;

                for (int i = 0; i <= opsize; i++) {
                    int64_t channel_mask = ch_mask;
                    aim_req.col_addr = host_req.col_addr + i;
                    for (int cnt = 0; cnt < channel_count; cnt++) {
                        uint8_t channel_id = FindFirstChannelIndex(channel_mask);
                        aim_req.AiM_req_id = AiM_req_id++;
                        aim_req.host_req_id = host_req.host_req_id;
                        apply_addr_mapp(aim_req, channel_id);
                        channel_pending_queues[channel_id].push(aim_req);
                    }
                }
                break;
            }
            case Opcode::ISR_WR_AFLUT:
                throw ConfigurationError("AiMDRAMSystem: ISR_WR_AFLUT not supported by now!");
            case Opcode::ISR_EWADD:
                break;
            case Opcode::ISR_SYNC:
            case Opcode::ISR_EOC: {
                aim_req.callback = callback;
                if (host_req.callback) {
                    int64_t all_ch_mask = (m_controllers.size() >= 32) ? -1LL : ((1LL << m_controllers.size()) - 1);
                    pending_callbacks.push({host_req, all_ch_mask});
                }
                for (int channel_id = 0; channel_id < (int)m_controllers.size(); channel_id++) {
                    aim_req.AiM_req_id = AiM_req_id++;
                    aim_req.host_req_id = host_req.host_req_id;
                    if ((int)aim_req.addr_vec.size() < m_num_levels)
                        aim_req.addr_vec.resize(m_num_levels, -1);
                    aim_req.addr_vec[m_dram->m_levels("channel")] = channel_id;
                    channel_pending_queues[channel_id].push(aim_req);
                }
                break;
            }
            default:
                m_logger->error("unknown command \n");
                break;
            }
            break;
        }
        case Type::Read: {
            switch (host_req.mem_access_region) {
            case MemAccessRegion::CFR:
            case MemAccessRegion::GPR:
                break;
            case MemAccessRegion::MEM: {
                Request aim_req = host_req;
                aim_req.AiM_req_id = AiM_req_id++;
                apply_addr_mapp(aim_req, aim_req.channel_mask);
                int channel_id = aim_req.addr_vec[m_dram->m_levels("channel")];
                channel_pending_queues[channel_id].push(aim_req);
                break;
            }
            default:
                throw ConfigurationError("AiMDRAMSystem: memory access region {}!", (int)host_req.mem_access_region);
            }
            break;
        }
        case Type::Write: {
            switch (host_req.mem_access_region) {
            case MemAccessRegion::CFR: {
                if (address_to_CFR.find(host_req.addr) == address_to_CFR.end())
                    throw ConfigurationError("AiMDRAMSystem: unknown CFR at location {}!", (int)host_req.addr);
                CFR_values[address_to_CFR[host_req.addr]] = host_req.data;
                break;
            }
            case MemAccessRegion::GPR:
                break;
            case MemAccessRegion::MEM: {
                Request aim_req = host_req;
                aim_req.AiM_req_id = AiM_req_id++;
                apply_addr_mapp(aim_req, aim_req.channel_mask);
                int channel_id = aim_req.addr_vec[m_dram->m_levels("channel")];
                channel_pending_queues[channel_id].push(aim_req);
                break;
            }
            default:
                throw ConfigurationError("AiMDRAMSystem: memory access region {}!", (int)host_req.mem_access_region);
            }
            break;
        }
        default:
            throw ConfigurationError("AiMDRAMSystem: unknown request type {}!", (int)host_req.type);
        }
    }

    void tick() override {
        // Phase 1: decompose one host_req from host_request_queue
        if (!host_request_queue.empty()) {
            decompose(host_request_queue.front());
            host_request_queue.pop();
        }

        // Phase 2: drain per-channel queues
        for (int channel_id = 0; channel_id < (int)m_controllers.size(); channel_id++) {
            while (!channel_pending_queues[channel_id].empty() && stalls[channel_id] == 0) {
                Request &aim = channel_pending_queues[channel_id].front();
                if (m_controllers[channel_id]->send(aim) == false)
                    break;
                if (is_blocking_aim_req(aim))
                    stalls[channel_id] += 1;
                channel_pending_queues[channel_id].pop();
            }
        }

        // Phase 3: tick DRAM and controllers
        if (m_clk % m_controllers[0]->get_clock_ratio() == 0) {
            m_dram->tick();
            for (auto controller : m_controllers) {
                controller->tick();
            }
        }

        m_clk++;
    };

    // Invoked by the controller when an aim_req with this->callback set completes.
    // Clears the channel's stall, advances last_completed_host_id[ch] to the
    // completing host_req's id, and walks the pending_callbacks FIFO firing
    // anything whose required channels have caught up.
    void receive(Request &req) {
        int channel_id = req.addr_vec[m_dram->m_levels("channel")];
        stalls[channel_id]--;

        if (req.host_req_id > last_completed_host_id[channel_id])
            last_completed_host_id[channel_id] = req.host_req_id;

        // Walk pending callbacks in FIFO order. A callback fires once every
        // channel in its required_channels mask has caught up to its host_req_id.
        // This barrier is race-free by construction: last_completed_host_id only
        // advances from real completions, so an unsent aim_req can never trip it.
        while (!pending_callbacks.empty()) {
            PendingCallback &pc = pending_callbacks.front();
            bool all_caught_up = true;
            int64_t mask = pc.required_channels;
            for (int ch = 0; ch < (int)m_controllers.size(); ch++) {
                if (mask & (1LL << ch)) {
                    if (last_completed_host_id[ch] < pc.host_req.host_req_id) {
                        all_caught_up = false;
                        break;
                    }
                }
            }
            if (!all_caught_up) break;
            Request host_req = pc.host_req;
            pending_callbacks.pop();
            if (host_req.callback)
                host_req.callback(host_req);
        }
    }

    float get_tCK() override {
        return m_dram->m_timing_vals("tCK_ps") / 1000.0f;
    }

    // const SpecDef& get_supported_requests() override {
    //   return m_dram->m_requests;
    // };
};

} // namespace Ramulator
