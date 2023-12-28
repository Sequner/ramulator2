#include "dram_controller/impl/plugin/mscache.h"
#include "dram_controller/controller.h"
#include "dram_controller/plugin.h"


namespace Ramulator {

class MithrilCache : public IControllerPlugin, public Implementation {
  RAMULATOR_REGISTER_IMPLEMENTATION(IControllerPlugin, MithrilCache, "MithrilCache", "MithrilCache.")

  private:
    IDRAM* m_dram = nullptr;

    int m_num_cache_entries = -1;
    int m_associativity = -1;
    int m_num_writeback_requests = 0;
    bool m_write_back_en = false;

    int m_write_miss_acts = 0;
    int m_read_miss_acts = 0;
    int m_mix_miss_acts = 0;

    int m_num_table_entries = -1;
    int m_activation_threshold = -1;
    int m_rfm_threshold = -1;

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
    int m_VRR_req_id = -1;

    int m_ACT_id = -1;
    int m_RD_id = -1;
    int m_WR_id = -1;
    int m_RDA_id = -1;
    int m_WRA_id = - 1;
    int m_PRE_id = -1;
    int m_VRR_id = -1;
    

    // per bank memory-side cache
    std::vector<MSCache> m_cache;

    // per bank activation count table
    // indexed using flattened <rank id, bank id>
    // e.g., if rank 0, bank 4, index is 4
    // if rank 1, bank 5, index is 16 (assuming 16 banks/rank) + 5
    std::vector<std::unordered_map<int, int>> m_activation_count_table;
    // Max_ptr per bank
    std::vector<int> m_max_ptr;
    // Min_ptr per bank
    std::vector<int> m_min_ptr;
    // RAA Counter per bank
    std::vector<int> m_raa_counter;

    // Flat bank ID to addr_vec
    std::unordered_map<int, AddrVec_t> m_bank_mapping;


  public:
    void init() override { 
      // MS Cache configs
      m_num_cache_entries = param<int>("num_cache_entries").required();
      m_associativity = param<int>("associativity").required();
      m_write_back_en = param<bool>("write_back_en").default_val(true);

      // Mithril+ configs
      m_num_table_entries = param<int>("num_table_entries").required();
      m_activation_threshold = param<int>("adaptive_threshold").required();
      m_rfm_threshold = param<int>("rfm_threshold").required();

      m_is_debug = param<bool>("debug").default_val(false);

      register_stat(m_num_writeback_requests).name("total_num_writeback_requests");
      register_stat(m_write_miss_acts).name("Total ACTs due to write");
      register_stat(m_read_miss_acts).name("Total ACTs due to read");
      register_stat(m_mix_miss_acts).name("Total ACTs due to mix of read/write");
    };

    void setup(IFrontEnd* frontend, IMemorySystem* memory_system) override {
      m_ctrl = cast_parent<IDRAMController>();
      m_dram = m_ctrl->m_dram;

      if (!m_dram->m_commands.contains("VRR")) {
        throw ConfigurationError("MithrilDDR4 is not compatible with the DRAM implementation that does not have Victim-Row-Refresh (VRR) command!");
      }

      m_channel_id = m_ctrl->m_channel_id;

      m_WR_req_id = m_dram->m_requests("write");
      m_VRR_req_id = m_dram->m_requests("victim-row-refresh");

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

      // Initialize bank act count tables
      for (int i = 0; i < m_num_banks_per_rank * m_num_ranks; i++) {
        std::unordered_map<int, int> table;
        for (int j = -m_num_rows_per_bank; j < -m_num_rows_per_bank + m_num_table_entries; j++) {
          table.insert(std::make_pair(j, 0));
        }
        m_activation_count_table.push_back(table);
      }

      // Initialize max pointers
      m_max_ptr = std::vector<int>(m_num_banks_per_rank * m_num_ranks, 0);

      // Initialize min pointers
      m_min_ptr = std::vector<int>(m_num_banks_per_rank * m_num_ranks, 0);

      // Initialize RFM counter
      m_raa_counter = std::vector<int>(m_num_banks_per_rank * m_num_ranks, 0);
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

    // Find max row id in the counter table
    void selectNewMaxRow(int flat_bank_id) {
      int max_count = -1;
      for (auto& [row, count] : m_activation_count_table[flat_bank_id]) {
        if (count > max_count) {
          m_max_ptr[flat_bank_id] = row;
          max_count = count;
        }
      }
    }

    // Find the value of the minptr
    int getMinCount(int flat_bank_id) {
      if (m_activation_count_table[flat_bank_id].size() == m_num_table_entries) {
        int minRow = m_min_ptr[flat_bank_id];
        return m_activation_count_table[flat_bank_id][minRow];
      }
      // return 1 if counter table is not full
      return 1;
    }

    // Perform RFM action for Mithril+
    void processRfm(int flat_bank_id) {
      // Reset RAA counter
      m_raa_counter[flat_bank_id] = 0;
      
      // check if the count exceeds the threshold in maxptr
      int curr_max_row = m_max_ptr[flat_bank_id];
      int curr_min_row = m_min_ptr[flat_bank_id];

      int diff = m_activation_count_table[flat_bank_id][curr_max_row] - m_activation_count_table[flat_bank_id][curr_min_row];

      if (diff >= m_activation_threshold) { // change to adaptive threshold
        if (m_is_debug) {
          std::cout << "Row " << curr_max_row << " in table " << flat_bank_id << " has exceeded the threshold!" << std::endl;
        }
        // if yes, schedule preventive refreshes
        AddrVec_t new_addr_vec = m_bank_mapping[flat_bank_id];
        new_addr_vec[m_row_level] = curr_max_row;
        Request vrr_req(new_addr_vec, m_VRR_req_id);
        m_ctrl->priority_send(vrr_req);

        // Set the value of max row to the value of min row
        m_activation_count_table[flat_bank_id][curr_max_row] = getMinCount(flat_bank_id);
        
        // Find new max row
        selectNewMaxRow(flat_bank_id);
      }
    }

    void updateCounters(ReqBuffer::iterator& req_it) {
      int flat_bank_id = getFlatBankId(req_it->addr_vec);
      int row_id = req_it->addr_vec[m_row_level];

      if (m_is_debug) {
        std::cout << "MithrilDDR4: ACT on row " << row_id << std::endl;
        std::cout << "  └  " << "rank: " << req_it->addr_vec[m_rank_level] << std::endl;
        std::cout << "  └  " << "bank_group: " << req_it->addr_vec[m_rank_level + 1] << std::endl;
        std::cout << "  └  " << "bank: " << req_it->addr_vec[m_bank_level] << std::endl;
        std::cout << "  └  " << "index: " << flat_bank_id << std::endl;
      }

      // Current min and max rows
      int maxRowId = m_max_ptr[flat_bank_id];
      int minRowId = m_min_ptr[flat_bank_id];

      // If current row id is not in the table 
      if (m_activation_count_table[flat_bank_id].find(row_id) == m_activation_count_table[flat_bank_id].end()) {

        // If counter table is not full
        if (m_activation_count_table[flat_bank_id].size() < m_num_table_entries) {
          m_activation_count_table[flat_bank_id][row_id] = 1;

          // In case it is the first entry, set max
          if (m_activation_count_table[flat_bank_id].size() == 1) {
            m_max_ptr[flat_bank_id] = row_id;
          }
        }
        // If counter table is full
        else {
          int minRowCount = getMinCount(flat_bank_id);
          m_activation_count_table[flat_bank_id].erase(minRowId); // Delete prev min row
          m_activation_count_table[flat_bank_id][row_id] = minRowCount + 1; // Replace it with current row
        }

        // Set minptr to newly added row
        m_min_ptr[flat_bank_id] = row_id;
      }
      else {
        // if row in table, increment its activation count
        m_activation_count_table[flat_bank_id][row_id] += 1;

        // if row larger than max, change maxptr
        if (m_activation_count_table[flat_bank_id][row_id] > m_activation_count_table[flat_bank_id][maxRowId]) {
          m_max_ptr[flat_bank_id] = row_id;
        }

        // if row_id == minptr, check if it's still min
        if (row_id == minRowId) {
          int row_act_count = m_activation_count_table[flat_bank_id][row_id];
          for (auto& [_row_id, _count] : m_activation_count_table[flat_bank_id]) {
            if (_count < row_act_count) {
              m_min_ptr[flat_bank_id] = _row_id;
              break;
            }
          }
        }
        
        if (m_is_debug) {
          std::cout << "Row " << row_id << " in table[" << flat_bank_id << "]" << std::endl;
          std::cout << "  └  " << "threshold: " << m_activation_threshold << std::endl;
          std::cout << "  └  " << "count: " << m_activation_count_table[flat_bank_id][row_id] << std::endl;
        }
      }
    }

    // Clear dirty buffer of the cache
    void clear_dirty_buffer(int bank_id) {
      std::vector<std::pair<int,int>> dirty_entries = m_cache[bank_id].get_dirty();
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
        // Increment RAA counter
        m_raa_counter[flat_bank_id] += 1;
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
        int status = m_cache[flat_bank_id].get_status();
        if (status > 0) {
          updateCounters(req_it);
          if (status == 1)
            m_read_miss_acts += 1;
          else if (status == 2)
            m_write_miss_acts += 1;
          else
            m_mix_miss_acts += 1;      
        }
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

      if (m_raa_counter[flat_bank_id] == m_rfm_threshold) {
        processRfm(flat_bank_id);
      }
    }
};

}       // namespace Ramulator
