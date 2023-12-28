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

  struct CacheSet {
    std::list<Line> set_lines; // LRU queue for the set. The head of the list is the least-recently-used way.
    std::unordered_map<int, std::list<Line>::iterator> set_mapping; // tag to cache line pointer
  };

  private:                 
    using DirtyList_t = std::unordered_map<Addr_t, int>;      // Dirty buffer

    std::unordered_map<int, CacheSet> m_cache_sets;

    bool m_wb_en = false; // write-back enabled?

    DirtyList_t m_dirty_entries;
    std::unordered_map<int, int> m_dirty_words_per_row;

    int m_activated_row = -1;
    int m_col_bits = -1;
    int m_num_dirty = 0;

    int m_status = false; // 0 is hit, 1 is miss due to read, 2 is miss due to write, 3 is mix

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
    MSCache(int latency, int num_entries, int associativity, int col_size, bool wb_en);
    
    void send_REF(int row_id);
    void send_ACT(int row_id) { assert(m_activated_row == -1); m_activated_row = row_id; };
    void send_PRE()           { m_activated_row = -1; };
    void send_access(int col_id, bool is_write);
    int get_status();

    std::vector<std::pair<int,int>> get_dirty();   // Get evicted dirty entries

  private:
    int get_index(Addr_t addr)  { return (addr >> m_index_offset) & m_index_mask; };
    Addr_t get_tag(Addr_t addr) { return (addr >> m_tag_offset); };

    CacheSet& get_set(Addr_t addr);
    void allocate_line(CacheSet& set, Line new_line);
    void evict_line(CacheSet& set);
    bool need_eviction(CacheSet& set, int tag);

    bool check_set_hit(CacheSet& set, int tag);

    void change_status(bool is_write);

    int get_addr(int col_id) { return (m_activated_row << m_col_bits + col_id); };
    int get_row(Addr_t addr) { return (addr >> m_col_bits); };
    int get_col(Addr_t addr) { return (addr & ((1 << m_col_bits) - 1)); };
};

}        // namespace Ramulator
