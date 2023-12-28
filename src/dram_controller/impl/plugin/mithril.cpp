#include <vector>
#include <unordered_map>
#include <limits>
#include <random>

#include "base/base.h"
#include "dram_controller/controller.h"
#include "dram_controller/plugin.h"

namespace Ramulator {

class MithrilDDR4 : public IControllerPlugin, public Implementation {
  RAMULATOR_REGISTER_IMPLEMENTATION(IControllerPlugin, MithrilDDR4, "MithrilDDR4", "MithrilDDR4.")

  private:
    IDRAM* m_dram = nullptr;

    int m_num_table_entries = -1;
    int m_activation_threshold = -1;
    int m_rfm_threshold = -1;
    bool m_is_debug = false;

    int m_ACT_id = -1;
    int m_VRR_req_id = -1;

    int m_channel_id = -1;

    int m_rank_level = -1;
    int m_bankgroup_level = -1;
    int m_bank_level = -1;
    int m_row_level = -1;

    int m_num_ranks = -1;
    int m_num_banks_per_rank = -1;
    int m_num_rows_per_bank = -1;

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
      m_num_table_entries = param<int>("num_table_entries").required();
      m_activation_threshold = param<int>("adaptive_threshold").required();
      m_rfm_threshold = param<int>("rfm_threshold").required();
      m_is_debug = param<bool>("debug").default_val(false);
    };

    void setup(IFrontEnd* frontend, IMemorySystem* memory_system) override {
      m_ctrl = cast_parent<IDRAMController>();
      m_dram = m_ctrl->m_dram;

      if (!m_dram->m_commands.contains("VRR")) {
        throw ConfigurationError("MithrilDDR4 is not compatible with the DRAM implementation that does not have Victim-Row-Refresh (VRR) command!");
      }

      m_ACT_id = m_dram->m_commands("ACT");
      m_VRR_req_id = m_dram->m_requests("victim-row-refresh");

      m_channel_id = m_ctrl->m_channel_id;

      m_rank_level = m_dram->m_levels("rank");
      m_bankgroup_level = m_dram->m_levels("bankgroup");
      m_bank_level = m_dram->m_levels("bank");
      m_row_level = m_dram->m_levels("row");

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

    void update(bool request_found, ReqBuffer::iterator& req_it) override {
      // If no request found by the MC, skip
      if (!request_found)
        return;

      // If command is not ACT, skip
      if (req_it->command != m_ACT_id)
        return;

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

      // Increment RAA counter
      m_raa_counter[flat_bank_id] += 1;

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

      if (m_raa_counter[flat_bank_id] == m_rfm_threshold) {
        processRfm(flat_bank_id);
      }
    }
};

}       // namespace Ramulator
