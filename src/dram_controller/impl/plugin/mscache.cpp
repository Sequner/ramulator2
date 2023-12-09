#include <iostream>
#include "dram_controller/impl/plugin/mscache.h"

namespace Ramulator {

MSCache::MSCache(int latency, int num_entries, int associativity, int col_size, bool wb_en, int wl_size):
                 m_latency(latency), m_num_entries(num_entries), m_associativity(associativity), 
                 m_col_bits(calc_log2(col_size)), m_wb_en(wb_en), m_wl_size(wl_size)
{
  m_set_size = num_entries / m_associativity;
  m_index_mask = m_set_size - 1;
  m_index_offset = 0;
  m_tag_offset = calc_log2(m_set_size) + m_index_offset;
  m_wl_en = m_wl_size > 0;
};

std::vector<std::pair<int,int>> MSCache::get_dirty() {
  std::vector<std::pair<int,int>> dirty_list;
  if (!m_wb_en) {
    return dirty_list;
  }

  if (m_num_dirty < 64) {
    return dirty_list;
  }

  for (auto& dirty_it: m_dirty_entries) {
    int row_id = get_row(dirty_it.first);
    if (dirty_it.second == 1) {
      int addr = dirty_it.first;
      dirty_list.push_back(std::make_pair(row_id, get_col(addr)));
      dirty_it.second = 0;
    }
  }

  m_num_dirty = 0;
  return dirty_list;
}

void MSCache::send_REF(int row_id) {
  // if the row in the white list, refresh the list
  if (check_white_list_hit(row_id)) {
    auto line_it = m_white_list.mapping[row_id];
    m_white_list.rows.erase(line_it);
  }
  else {
    if (m_white_list.rows.size() >= m_wl_size) {
      auto victim = m_white_list.rows.front();
      m_white_list.mapping.erase(victim);
      m_white_list.rows.pop_front();
    }
  }
  m_white_list.rows.push_back(row_id);
  m_white_list.mapping[row_id] = --m_white_list.rows.end();
}

void MSCache::send_access(int col_id, bool is_write) {
  int addr = get_addr(col_id);
  CacheSet& set = get_set(addr);

  // If write-through, set status as miss to issue DRAM ACT
  if (!m_wb_en && is_write) {
    change_status(is_write);
    return;
  }

  // Only when write-back is on, if request is in dirty buffer
  if (m_wb_en) {
    if (auto dirty_it = m_dirty_entries.find(addr); dirty_it != m_dirty_entries.end()) {
      // Writing back entry to DRAM
      if (dirty_it->second == 0 && is_write) {
        change_status(is_write);
        m_dirty_entries.erase(dirty_it);
      }

      return;
    }
  }

  // Check set for hit / miss
  int addr_tag = get_tag(addr);

  // Cache hit
  if (check_set_hit(set, addr_tag)) {
    // Cache hit
    // Update LRU
    auto line_it = set.set_mapping[get_tag(addr)];
    Line new_line = {addr, addr_tag, line_it->dirty || is_write};
    set.set_lines.push_back(new_line);
    set.set_mapping[addr_tag] = --set.set_lines.end();
    set.set_lines.erase(line_it);
  }
  // Cache miss
  else {
    if (!m_wl_en || check_white_list_hit(m_activated_row)) {
      // If read miss and current status is hit, set to miss
      change_status(is_write && !m_wb_en);

      Line new_line = {addr, addr_tag, is_write};
      allocate_line(set, new_line);
    }
    else {
      change_status(is_write); // write-through, no allocate
    }
  }
}

MSCache::CacheSet& MSCache::get_set(Addr_t addr) {
  int set_index = get_index(addr);
  if (m_cache_sets.find(set_index) == m_cache_sets.end()) {
    m_cache_sets.insert(std::make_pair(set_index, MSCache::CacheSet()));
  }
  return m_cache_sets[set_index];
}

void MSCache::allocate_line(CacheSet& set, Line new_line) {
  // Check if we need to evict any line
  if (need_eviction(set, new_line.tag)) {
    evict_line(set);
  }

  // Allocate new cache line and return an iterator to it
  set.set_lines.push_back(new_line);
  set.set_mapping[new_line.tag] = --set.set_lines.end();
}

bool MSCache::need_eviction(CacheSet& set, int tag) {
  if (std::find_if(set.set_lines.begin(), set.set_lines.end(), 
            [tag, this](Line l) { return (tag == l.tag); }) 
      != set.set_lines.end()) {
    // Due to MSHR, the program can't reach here. Just for checking
    assert(false);
    return false;
  } 
  else {
    if (set.set_lines.size() < m_associativity) {
      return false;
    } else {
      return true;
    }
  }
}

void MSCache::evict_line(CacheSet& set) {
  auto victim_it = set.set_lines.begin();
  // Add addr to dirty buffer if victim line is dirty
  if (victim_it->dirty) {
    m_num_dirty += 1;
    m_dirty_entries.insert(std::make_pair(victim_it->addr, 1));
  }

  set.set_mapping.erase(victim_it->tag);
  set.set_lines.pop_front();
}

bool MSCache::check_set_hit(CacheSet& set, int tag) {
  return set.set_mapping.contains(tag);
}

bool MSCache::check_white_list_hit(int row_id) {
  return m_white_list.mapping.contains(row_id);
}

// Read cache status (hit/miss) and reset it
int MSCache::get_status() {
  int res = m_status;
  m_status = 0;
  return res;
}

void MSCache::change_status(bool is_write) {
  if (m_status == 0)
    m_status = 1 + is_write;
  else if ((m_status == 1 && is_write) || (m_status == 2 && !is_write))
    m_status = 3;
}

}        // namespace Ramulator