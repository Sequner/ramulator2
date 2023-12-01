#include <vector>
#include <list>
#include <unordered_map>
#include <iostream>
#include <fstream>

#include "base/base.h"

namespace Ramulator {

class MSCache {
  struct Line {
    Addr_t addr = -1;
    Addr_t tag = -1;
    bool dirty = false;
  };

  private:
    using CacheSet_t  = std::list<Line>;                      // LRU queue for the set. The head of the list is the least-recently-used way.
    using DirtyList_t = std::unordered_map<Addr_t, int>;      // Dirty buffer

    std::unordered_map<int, CacheSet_t> m_cache_sets;
    DirtyList_t m_dirty_entries;

    int m_activated_row = -1;
    int m_col_bits = -1;

  public:
    int m_latency;

    int m_num_entries;
    int m_associativity;
    int m_set_size;

    Addr_t m_tag_mask;
    int m_tag_offset;

    Addr_t m_index_mask;
    int m_index_offset;

  public:
    MSCache(int latency, int num_entries, int associativity, int col_size);
    
    void send_ACT(int row_id) { m_activated_row = row_id; };
    void send_PRE() { m_activated_row = -1; };
    void send_access(int col_id, bool is_write);

    std::vector<std::pair<int,int>> get_dirty();   // Get evicted dirty entries

  private:
    int get_index(Addr_t addr)  { return (addr >> m_index_offset) & m_index_mask; };
    Addr_t get_tag(Addr_t addr) { return (addr >> m_tag_offset); };

    CacheSet_t& get_set(Addr_t addr);
    CacheSet_t::iterator allocate_line(CacheSet_t& set, Addr_t addr);
    bool need_eviction(const CacheSet_t& set, Addr_t addr);
    void evict_line(CacheSet_t& set, CacheSet_t::iterator victim_it);

    CacheSet_t::iterator check_set_hit(CacheSet_t& set, Addr_t addr);

    int get_addr(int col_id) { return (m_activated_row << m_col_bits + col_id); };
    int get_row(Addr_t addr) { return (addr >> m_col_bits); };
    int get_col(Addr_t addr) { return (addr & ((1 << m_col_bits) - 1)); };
};

}        // namespace Ramulator
