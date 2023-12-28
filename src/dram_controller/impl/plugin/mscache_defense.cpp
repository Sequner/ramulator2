#include "dram_controller/impl/plugin/mscache.h"
#include "dram_controller/controller.h"
#include "dram_controller/plugin.h"


namespace Ramulator {

class MSCache_Defense : public IControllerPlugin, public Implementation {
  RAMULATOR_REGISTER_IMPLEMENTATION(IControllerPlugin, MSCache_Defense, "MSCache", "MSCache.")

  private:
    IDRAM* m_dram = nullptr;

    int m_num_cache_entries = -1;
    int m_associativity = -1;
    int m_num_writeback_requests = 0;
    bool m_write_back_en = false;

    bool m_is_debug = false;

    int m_channel_id = -1;

    int m_rank_level = -1;
    int m_bankgroup_level = -1;
    int m_bank_level = -1;
    int m_row_level = -1;
    int m_col_level = -1;

    int m_num_ranks = -1;
    int m_num_banks_per_rank = -1;
    int m_num_rows_per_bank = -1;

    int m_WR_req_id = -1;

    int m_ACT_id = -1;
    int m_RD_id = -1;
    int m_WR_id = -1;
    int m_RDA_id = -1;
    int m_WRA_id = - 1;
    int m_PRE_id = -1;
    int m_VRR_id = -1;

    // per bank memory-side cache
    std::vector<MSCache> m_cache;

    // Flat bank ID to addr_vec
    std::unordered_map<int, AddrVec_t> m_bank_mapping;


  public:
    void init() override { 
      m_num_cache_entries = param<int>("num_cache_entries").required();
      m_associativity = param<int>("associativity").required();
      m_write_back_en = param<bool>("write_back_en").default_val(true);

      m_is_debug = param<bool>("debug").default_val(false);
      register_stat(m_num_writeback_requests).name("total_num_writeback_requests");
    };

    void setup(IFrontEnd* frontend, IMemorySystem* memory_system) override {
      m_ctrl = cast_parent<IDRAMController>();
      m_dram = m_ctrl->m_dram;

      m_channel_id = m_ctrl->m_channel_id;

      m_WR_req_id = m_dram->m_requests("write");

      m_ACT_id = m_dram->m_commands("ACT");
      m_RD_id = m_dram->m_commands("RD");
      m_WR_id = m_dram->m_commands("WR");
      m_RDA_id = m_dram->m_commands("RDA");
      m_WRA_id = m_dram->m_commands("WRA");
      m_PRE_id = m_dram->m_commands("PRE");
      m_VRR_id = m_dram->m_commands("VRR");

      m_rank_level = m_dram->m_levels("rank");
      m_bankgroup_level = m_dram->m_levels("bankgroup");
      m_bank_level = m_dram->m_levels("bank");
      m_row_level = m_dram->m_levels("row");
      m_col_level = m_dram->m_levels("column");

      m_num_ranks = m_dram->get_level_size("rank");
      int m_num_bankgroups = m_dram->get_level_size("bankgroup");
      int m_num_banks = m_dram->get_level_size("bank");
      m_num_banks_per_rank = m_num_bankgroups == -1 ? 
                             m_num_banks : 
                             m_num_bankgroups * m_num_banks;
      m_num_rows_per_bank = m_dram->get_level_size("row");

      // Initialize flat bank ID to addr vec mapping
      AddrVec_t addr_vec = std::vector<int>(m_dram->m_levels.size(), 0); 
      for (int rank = 0; rank < m_num_ranks; rank++) {
        addr_vec[m_rank_level] = rank;
        if (m_num_bankgroups == -1) {
          for (int bank = 0; bank < m_num_banks_per_rank; bank++) {
            addr_vec[m_bank_level] = bank;
            m_bank_mapping.insert({rank * m_num_banks + bank, addr_vec});
          }
        }
        else {
          for (int bankgroup = 0; bankgroup < m_num_bankgroups; bankgroup++) {
            addr_vec[m_bankgroup_level] = bankgroup;
            for (int banks = 0; banks < m_num_banks; banks++) {
              addr_vec[m_bank_level] = banks;
              m_bank_mapping.insert({rank * m_num_banks_per_rank + bankgroup * m_num_banks + banks, addr_vec});
            }
          }
        }
      }
      // Initialize bank caches
      for (int i = 0; i < m_num_banks_per_rank * m_num_ranks; i++) {
        m_cache.push_back(MSCache(0, m_num_cache_entries, m_associativity, 64, m_write_back_en));
      }
    };

    // Convert rank, bankgroup, bank to bank id
    int getFlatBankId(AddrVec_t addr_vec) {
      int flat_bank_id = addr_vec[m_bank_level];
      int accumulated_dimension = 1;
      for (int i = m_bank_level - 1; i >= m_rank_level; i--) {
        accumulated_dimension *= m_dram->m_organization.count[i + 1];
        flat_bank_id += addr_vec[i] * accumulated_dimension;
      }

      return flat_bank_id;
    }

    // Clear dirty buffer of the cache
    void clear_dirty_buffer(int bank_id) {
      std::vector<std::pair<int,int>> dirty_entries = m_cache[bank_id].get_dirty();
      // std::ranges::sort(dirty_entries, [](const std::pair<int,int> &a, const std::pair<int,int> &b)
      // {
      //   return a.first < b.first;
      // });
      AddrVec_t addr_vec = m_bank_mapping[bank_id];
      for (auto& entry_it: dirty_entries) {
        addr_vec[m_row_level] = entry_it.first;
        addr_vec[m_col_level] = entry_it.second;
        Request wr_req(addr_vec, m_WR_req_id);
        m_ctrl->priority_send(wr_req);
        m_num_writeback_requests++;
      }
    }

    std::string get_cmd_name(int cmd_id) {
      if (cmd_id == m_ACT_id) {
        return "ACT";
      }
      if (cmd_id == m_RD_id) {
        return "READ";
      }
      if (cmd_id == m_WR_id) {
        return "WRITE";
      }
      if (cmd_id == m_RDA_id) {
        return "READ+PRE";
      }
      if (cmd_id == m_WRA_id) {
        return "WRITE+PRE";
      }
      if (cmd_id == m_PRE_id) {
        return "PRE";
      }
      if (cmd_id == m_VRR_id) {
        return "VRR";
      }
      return "";
    }

    void update(bool request_found, ReqBuffer::iterator& req_it) override {
      // If no request found by the MC, skip
      if (!request_found)
        return;
      int flat_bank_id = getFlatBankId(req_it->addr_vec);

      if (req_it->command == m_ACT_id) {
        m_cache[flat_bank_id].send_ACT(req_it->addr_vec[m_row_level]);
      } else if (req_it->command == m_RD_id) {
        m_cache[flat_bank_id].send_access(req_it->addr_vec[m_col_level], false);
      } else if (req_it->command == m_WR_id) {
        m_cache[flat_bank_id].send_access(req_it->addr_vec[m_col_level], true);
      } else if (req_it->command == m_PRE_id || req_it->command == m_RDA_id || req_it->command == m_WRA_id) {
        if (req_it->command != m_PRE_id) {
          bool is_write = (req_it->command == m_WRA_id);
          m_cache[flat_bank_id].send_access(req_it->addr_vec[m_col_level], is_write);
        }
        m_cache[flat_bank_id].send_PRE();
        
        clear_dirty_buffer(flat_bank_id);        
      }

      if (m_is_debug) {
        std::string req_name = get_cmd_name(req_it->command);
        if (req_name.size() > 0) {
          std::cout << "Cache: " << req_name << " request" << std::endl;
          std::cout << "  └  " << "rank: " << req_it->addr_vec[m_rank_level] << std::endl;
          std::cout << "  └  " << "bank_group: " << req_it->addr_vec[m_rank_level + 1] << std::endl;
          std::cout << "  └  " << "bank: " << req_it->addr_vec[m_bank_level] << std::endl;
          std::cout << "  └  " << "index: " << flat_bank_id << std::endl;
          std::cout << "  └  " << "row: " << req_it->addr_vec[m_row_level] << std::endl;
          std::cout << "  └  " << "col: " << req_it->addr_vec[m_col_level] << std::endl;
        }
      }
    }
};

}       // namespace Ramulator
