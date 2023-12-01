#include <iostream>
#include "dram_controller/impl/plugin/mscache.h"

namespace Ramulator {

MSCache::MSCache(int latency, int num_entries, int associativity, int col_size):
                 m_latency(latency), m_num_entries(num_entries), m_associativity(associativity), 
                 m_col_bits(calc_log2(col_size))
{
  m_set_size = num_entries / m_associativity;
  m_index_mask = m_set_size - 1;
  m_index_offset = 0;
  m_tag_offset = calc_log2(m_set_size) + m_index_offset;
};

std::vector<std::pair<int,int>> MSCache::get_dirty() {
  std::vector<std::pair<int,int>> dirty_list;
  for (auto& dirty_it: m_dirty_entries) {
    if (dirty_it.second == 1) {
      int addr = dirty_it.first;
      dirty_list.push_back(std::make_pair(get_row(addr), get_col(addr)));
      dirty_it.second = 0;
    }
  }

  return dirty_list;
}

void MSCache::send_access(int col_id, bool is_write) {
  int addr = get_addr(col_id);
  CacheSet_t& set = get_set(addr);

  // If request is in dirty buffer
  if (auto dirty_it = m_dirty_entries.find(addr); dirty_it != m_dirty_entries.end()) {
    // Writing back entry to DRAM
    if (dirty_it->second == 0 && is_write) {
      m_dirty_entries.erase(addr);
    } else if (dirty_it->second == 1) {
      auto newline_it = allocate_line(set, addr);
      newline_it->dirty = 1;
      m_dirty_entries.erase(addr);
    }

    return;
  }

  // Check set for hit / miss
  if (auto line_it = check_set_hit(set, addr); line_it != set.end()) {
    // Cache hit
    // Update LRU
    set.push_back({addr, get_tag(addr), line_it->dirty || is_write});
    set.erase(line_it);
  } else {
    // Cache miss
    auto newline_it = allocate_line(set, addr);
    newline_it->dirty = is_write;
  }
}

MSCache::CacheSet_t& MSCache::get_set(Addr_t addr) {
  int set_index = get_index(addr);
  if (m_cache_sets.find(set_index) == m_cache_sets.end()) {
    m_cache_sets.insert(make_pair(set_index, std::list<Line>()));
  }
  return m_cache_sets[set_index];
}

MSCache::CacheSet_t::iterator MSCache::allocate_line(CacheSet_t& set, Addr_t addr) {
  // Check if we need to evict any line
  if (need_eviction(set, addr)) {
    evict_line(set, set.begin());
  }

  // Allocate new cache line and return an iterator to it
  set.push_back({addr, get_tag(addr)});
  return --set.end();
}

bool MSCache::need_eviction(const CacheSet_t& set, Addr_t addr) {
  if (std::find_if(set.begin(), set.end(), 
            [addr, this](Line l) { return (get_tag(addr) == l.tag); }) 
      != set.end()) {
    // Due to MSHR, the program can't reach here. Just for checking
    assert(false);
    return false;
  } 
  else {
    if (set.size() < m_associativity) {
      return false;
    } else {
      return true;
    }
  }
}

void MSCache::evict_line(CacheSet_t& set, CacheSet_t::iterator victim_it) {
  // Add addr to dirty buffer if victim line is dirty
  if (victim_it->dirty) {
    m_dirty_entries.insert(std::make_pair(victim_it->addr, 1));
  }

  set.erase(victim_it);
}

MSCache::CacheSet_t::iterator MSCache::check_set_hit(CacheSet_t& set, Addr_t addr) {
  return std::find_if(set.begin(), set.end(), [addr, this](Line l){return (l.tag == get_tag(addr));});
}

}        // namespace Ramulator