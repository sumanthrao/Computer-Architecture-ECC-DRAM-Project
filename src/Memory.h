#ifndef __MEMORY_H
#define __MEMORY_H

#include "Config.h"
#include "DRAM.h"
#include "Request.h"
#include "Controller.h"
#include "SpeedyController.h"
#include "Statistics.h"
#include "GDDR5.h"
#include "HBM.h"
#include "LPDDR3.h"
#include "LPDDR4.h"
#include "WideIO2.h"
#include "DSARP.h"
#include <vector>
#include <functional>
#include <cmath>
#include <cassert>
#include <tuple>
#include <unordered_set>

using namespace std;

typedef vector<unsigned int> MapSrcVector;
typedef map<unsigned int, MapSrcVector > MapSchemeEntry;
typedef map<unsigned int, MapSchemeEntry> MapScheme;

namespace ramulator
{

class MemoryBase{
public:
    MemoryBase() {}
    virtual ~MemoryBase() {}
    virtual double clk_ns() = 0;
    virtual void tick() = 0;
    virtual bool send(Request req) = 0;
    virtual int pending_requests() = 0;
    virtual void finish(void) = 0;
    virtual long page_allocator(long addr, int coreid, bool is_write, bool instr_limit) = 0;
    virtual void record_core(int coreid) = 0;
    virtual void set_high_writeq_watermark(const float watermark) = 0;
    virtual void set_low_writeq_watermark(const float watermark) = 0;
};

template <class T, template<typename> class Controller = Controller >
class Memory : public MemoryBase
{
protected:
  ScalarStat dram_capacity;
  ScalarStat num_dram_cycles;
  ScalarStat num_incoming_requests;
  VectorStat num_read_requests;
  VectorStat num_write_requests;
  ScalarStat ramulator_active_cycles;
  VectorStat incoming_requests_per_channel;
  VectorStat incoming_read_reqs_per_channel;

  ScalarStat physical_page_replacement;
  ScalarStat physical_pages_remaining;
  ScalarStat num_memory_write_requests;
  ScalarStat dirty_write_physical_pages;
  ScalarStat total_physical_pages;
  ScalarStat total_dirty_pages;
  ScalarStat maximum_bandwidth;
  ScalarStat in_queue_req_num_sum;
  ScalarStat in_queue_read_req_num_sum;
  ScalarStat in_queue_write_req_num_sum;
  ScalarStat in_queue_req_num_avg;
  ScalarStat in_queue_read_req_num_avg;
  ScalarStat in_queue_write_req_num_avg;
  ScalarStat max_dirty_pages;
  ScalarStat sum_dirty_pages;

#ifndef INTEGRATED_WITH_GEM5
  VectorStat record_read_requests;
  VectorStat record_write_requests;
#endif

  long max_address;
  MapScheme mapping_scheme;
  
public:
    enum class Type {
        ChRaBaRoCo,
        RoBaRaCoCh,
        MAX,
    } type = Type::ChRaBaRoCo;

    enum class Translation {
      None,
      Random,
      MAX,
    } translation = Translation::None;

    std::map<string, Translation> name_to_translation = {
      {"None", Translation::None},
      {"Random", Translation::Random},
    };

    vector<int> free_physical_pages;
    long free_physical_pages_remaining;
    /* Idea: store the page mapping along with whether its dirty */
    map<pair<int, long>, pair<long, int>> page_translation;
    unordered_set<long> dirty_physical_pages;
    /* add book keeping code to know which mode to use */
    enum class CREAM_MODE { NORMAL = 0, RANK_SUBSET, WRAP_AROUND, BOUNDARY_SUBSET, PIM} cream_mode = CREAM_MODE::NORMAL;

    vector<Controller<T>*> ctrls;
    T * spec;
    vector<int> addr_bits;
    string mapping_file;
    bool use_mapping_file;
    bool dump_mapping;
    int global_req_count = 0;

    int tx_bits;

    Memory(const Config& configs, vector<Controller<T>*> ctrls)
        : ctrls(ctrls),
          spec(ctrls[0]->channel->spec),
          addr_bits(int(T::Level::MAX))
    {
        // make sure 2^N channels/ranks
        // TODO support channel number that is not powers of 2
        int *sz = spec->org_entry.count;
        assert((sz[0] & (sz[0] - 1)) == 0);
        assert((sz[1] & (sz[1] - 1)) == 0);
        // validate size of one transaction
        int tx = (spec->prefetch_size * spec->channel_width / 8);
        tx_bits = calc_log2(tx);
        assert((1<<tx_bits) == tx);
        
        // Parsing mapping file and initialize mapping table
        use_mapping_file = false;
        dump_mapping = false;
        if (spec->standard_name.substr(0, 4) == "DDR3"){
            if (configs["mapping"] != "defaultmapping"){
              init_mapping_with_file(configs["mapping"]);
              // dump_mapping = true;
              use_mapping_file = true;
            }
        }
        // If hi address bits will not be assigned to Rows
        // then the chips must not be LPDDRx 6Gb, 12Gb etc.
        if (type != Type::RoBaRaCoCh && spec->standard_name.substr(0, 5) == "LPDDR")
            assert((sz[int(T::Level::Row)] & (sz[int(T::Level::Row)] - 1)) == 0);

        /** CUSTOM CODE : CREAM */
        cream_mode = CREAM_MODE::BOUNDARY_SUBSET;

        max_address = spec->channel_width / (8); // 8

        for (unsigned int lev = 0; lev < addr_bits.size(); lev++) {
            addr_bits[lev] = calc_log2(sz[lev]);
            /* For COL add customization code to process CREAM*/
            if (lev == 4) {
                // if we are using the ECC portion of RAM, then max address will be different
                if(cream_mode == CREAM_MODE::BOUNDARY_SUBSET) {
                    max_address *= sz[lev] * (9/8); // ((sz[lev] / 2) + (sz[lev]/(2*8))); // sz[4] represents the col count 
                } else {
                    max_address *= sz[lev];  // we have one addiional chip of capacity
                }
            /* For BOUNDARY method, diving each row */
            } else if (lev == 3) {
                if(cream_mode == CREAM_MODE::BOUNDARY_SUBSET) {
                    max_address *= sz[lev]/2; 
                } else {
                    max_address *= sz[lev]; 
                }
            } else {
                max_address *= sz[lev];
            }
        }

        if (cream_mode == CREAM_MODE::BOUNDARY_SUBSET) {
            max_address = 75091456;
        }
        addr_bits[int(T::Level::MAX) - 1] -= calc_log2(spec->prefetch_size);

        // Initiating translation
        if (configs.contains("translation")) {
          translation = name_to_translation[configs["translation"]];
        }
        if (translation != Translation::None) {
          // construct a list of available pages
          // TODO: this should not assume a 4KB page!
          /** CUSTOM init the pages here */
          free_physical_pages_remaining = max_address >> 12;
          physical_pages_remaining = max_address >> 12;
          total_physical_pages = max_address >> 12;
          dirty_write_physical_pages = 0;
          dirty_write_physical_pages = 0;
          //fprintf(stderr, "size : %ld\n", free_physical_pages_remaining);
          free_physical_pages.resize(free_physical_pages_remaining, -1);
        }

        dram_capacity
            .name("dram_capacity")
            .desc("Number of bytes in simulated DRAM")
            .precision(0)
            ;
        dram_capacity = max_address;
        
        num_dram_cycles
            .name("dram_cycles")
            .desc("Number of DRAM cycles simulated")
            .precision(0)
            ;
        num_incoming_requests
            .name("incoming_requests")
            .desc("Number of incoming requests to DRAM")
            .precision(0)
            ;
        num_read_requests
            .init(configs.get_core_num())
            .name("read_requests")
            .desc("Number of incoming read requests to DRAM per core")
            .precision(0)
            ;
        num_write_requests
            .init(configs.get_core_num())
            .name("write_requests")
            .desc("Number of incoming write requests to DRAM per core")
            .precision(0)
            ;
        incoming_requests_per_channel
            .init(sz[int(T::Level::Channel)])
            .name("incoming_requests_per_channel")
            .desc("Number of incoming requests to each DRAM channel")
            ;
        incoming_read_reqs_per_channel
            .init(sz[int(T::Level::Channel)])
            .name("incoming_read_reqs_per_channel")
            .desc("Number of incoming read requests to each DRAM channel")
            ;

        ramulator_active_cycles
            .name("ramulator_active_cycles")
            .desc("The total number of cycles that the DRAM part is active (serving R/W)")
            .precision(0)
            ;
        physical_page_replacement
            .name("physical_page_replacement")
            .desc("The number of times that physical page replacement happens.")
            .precision(0)
            ;
        physical_pages_remaining
            .name("physical_pages_remaining")
            .desc("[custom] The number of free physical pages in memory")
            .precision(0)
            ;
        total_physical_pages
            .name("total_physical_pages")
            .desc("[custom] The number of total physical pages in memory")
            .precision(0)
            ;
        total_dirty_pages
            .name("total_dirty_pages")
            .desc("[custom] The number of total dirty physical pages")
            .precision(0)
            ;
        dirty_write_physical_pages
            .name("dirty_write_physical_pages")
            .desc("[custom] The number of write dirty physical pages")
            .precision(0)
            ;
        maximum_bandwidth
            .name("maximum_bandwidth")
            .desc("The theoretical maximum bandwidth (Bps)")
            .precision(0)
            ;

        sum_dirty_pages
            .name("sum_dirty_pages")
            .desc("sum_dirty_pages")
            .precision(0)
            ;

        max_dirty_pages
            .name("max_dirty_pages")
            .desc("max_dirty_pages")
            .precision(0)
            ;
        in_queue_req_num_sum
            .name("in_queue_req_num_sum")
            .desc("Sum of read/write queue length")
            .precision(0)
            ;
        in_queue_read_req_num_sum
            .name("in_queue_read_req_num_sum")
            .desc("Sum of read queue length")
            .precision(0)
            ;
        in_queue_write_req_num_sum
            .name("in_queue_write_req_num_sum")
            .desc("Sum of write queue length")
            .precision(0)
            ;
        in_queue_req_num_avg
            .name("in_queue_req_num_avg")
            .desc("Average of read/write queue length per memory cycle")
            .precision(6)
            ;
        in_queue_read_req_num_avg
            .name("in_queue_read_req_num_avg")
            .desc("Average of read queue length per memory cycle")
            .precision(6)
            ;
        in_queue_write_req_num_avg
            .name("in_queue_write_req_num_avg")
            .desc("Average of write queue length per memory cycle")
            .precision(6)
            ;
        num_memory_write_requests
            .name("num_memory_write_requests")
            .desc("num_memory_write_requests")
            .precision(6)
            ;
#ifndef INTEGRATED_WITH_GEM5
        record_read_requests
            .init(configs.get_core_num())
            .name("record_read_requests")
            .desc("record read requests for this core when it reaches request limit or to the end")
            ;

        record_write_requests
            .init(configs.get_core_num())
            .name("record_write_requests")
            .desc("record write requests for this core when it reaches request limit or to the end")
            ;
#endif

    }

    ~Memory()
    {
        for (auto ctrl: ctrls)
            delete ctrl;
        delete spec;
    }

    double clk_ns()
    {
        return spec->speed_entry.tCK;
    }

    void record_core(int coreid) {
#ifndef INTEGRATED_WITH_GEM5
      record_read_requests[coreid] = num_read_requests[coreid];
      record_write_requests[coreid] = num_write_requests[coreid];
#endif
      for (auto ctrl : ctrls) {
        ctrl->record_core(coreid);
      }
    }

    void tick()
    {
        ++num_dram_cycles;
        int cur_que_req_num = 0;
        int cur_que_readreq_num = 0;
        int cur_que_writereq_num = 0;
        for (auto ctrl : ctrls) {
          cur_que_req_num += ctrl->readq.size() + ctrl->writeq.size() + ctrl->pending.size();
          cur_que_readreq_num += ctrl->readq.size() + ctrl->pending.size();
          cur_que_writereq_num += ctrl->writeq.size();
        }
        in_queue_req_num_sum += cur_que_req_num;
        in_queue_read_req_num_sum += cur_que_readreq_num;
        in_queue_write_req_num_sum += cur_que_writereq_num;

        bool is_active = false;
        for (auto ctrl : ctrls) {
          is_active = is_active || ctrl->is_active();
          ctrl->tick();
        }
        if (is_active) {
          ramulator_active_cycles++;
        }
    }

    bool send(Request req)
    {
        req.addr_vec.resize(addr_bits.size());
        long addr = req.addr;
        int coreid = req.coreid;

        // Each transaction size is 2^tx_bits, so first clear the lowest tx_bits bits
        clear_lower_bits(addr, tx_bits);

        if (use_mapping_file){
            apply_mapping(addr, req.addr_vec);
        }
        else {
            switch(int(type)){
                case int(Type::ChRaBaRoCo):
                    for (int i = addr_bits.size() - 1; i >= 0; i--){
                        // here we need to check for the COL 
                        auto val = sum_dirty_pages.value() ;
                        val += total_dirty_pages.value();
                        sum_dirty_pages = val;

                        if(i == 4 && (cream_mode == CREAM_MODE::RANK_SUBSET )) {
                            int chip_index = addr & (( 1 << 4 ) - 1);
                            // if this is the 8th chip
                            // make it 9 if utilizing chip 8 or else keep it 8
                            int NUM_CHIPS; 
                            if (cream_mode == CREAM_MODE::RANK_SUBSET) {
                                NUM_CHIPS = 9;
                            } else {
                                NUM_CHIPS = 8;
                            }
                            chip_index = chip_index % NUM_CHIPS;
                    
                            // (bits + 1) to represent the increased capacity
                            auto mask = ((1 << (addr_bits[i])) - 1);
                            // some complicated 2am bit math
                            mask = mask &  ~(( 1 << 4 ) - 1);
                            mask = mask | chip_index;
                            int lbits = addr & mask;
                            req.addr_vec[i] = lbits;

                            // slice lower bits anyways to clear them
                            auto og = slice_lower_bits(addr, addr_bits[i]);
                            
                            //cout << "og addr: " << og << " updated addr: " << req.addr_vec[i] << std::endl;
                        } else if (cream_mode == CREAM_MODE::BOUNDARY_SUBSET){
                            if (i == 3) {
                                req.addr_vec[i] = slice_lower_bits(addr, addr_bits[i]);
                                auto row_add = 1024 + 121;
                                req.addr_vec[i] = req.addr_vec[i] % row_add;
                            }
                        } else {
                            req.addr_vec[i] = slice_lower_bits(addr, addr_bits[i]);
                        } 
                        auto cur_addr = req.addr;
                        cur_addr = cur_addr >> 12;
                        auto page = page_translation.find(std::make_pair(coreid, cur_addr)); 
                       
                        if (page != page_translation.end()) {
                            auto phy_page = (*page).second.first;
                            if (dirty_physical_pages.find(phy_page) != dirty_physical_pages.end()) {
                                // set dirty to true;
                                req.dirty = 1;
                            }
                        }
                    }
                    break;
                case int(Type::RoBaRaCoCh):
                    req.addr_vec[0] = slice_lower_bits(addr, addr_bits[0]);
                    req.addr_vec[addr_bits.size() - 1] = slice_lower_bits(addr, addr_bits[addr_bits.size() - 1]);
                    for (int i = 1; i <= int(T::Level::Row); i++)
                        req.addr_vec[i] = slice_lower_bits(addr, addr_bits[i]);
                    break;
                default:
                    assert(false);
            }
        }

        if(ctrls[req.addr_vec[0]]->enqueue(req)) {
            // tally stats here to avoid double counting for requests that aren't enqueued
            ++num_incoming_requests;
            if (req.type == Request::Type::READ) {
              ++num_read_requests[coreid];
              ++incoming_read_reqs_per_channel[req.addr_vec[int(T::Level::Channel)]];
              // added counter to support wrap around
              req.req_number = global_req_count++;
            }
            if (req.type == Request::Type::WRITE) {
              ++num_write_requests[coreid];
            }
            ++incoming_requests_per_channel[req.addr_vec[int(T::Level::Channel)]];
            return true;
        }

        return false;
    }
    
    void init_mapping_with_file(string filename){
        ifstream file(filename);
        assert(file.good() && "Bad mapping file");
        // possible line types are:
        // 0. Empty line
        // 1. Direct bit assignment   : component N   = x
        // 2. Direct range assignment : component N:M = x:y
        // 3. XOR bit assignment      : component N   = x y z ...
        // 4. Comment line            : # comment here
        string line;
        char delim[] = " \t";
        while (getline(file, line)) {
            short capture_flags = 0;
            int level = -1;
            int target_bit = -1, target_bit2 = -1;
            int source_bit = -1, source_bit2 = -1;
            // cout << "Processing: " << line << endl;
            bool is_range = false;
            while (true) { // process next word
                size_t start = line.find_first_not_of(delim);
                if (start == string::npos) // no more words
                    break;
                size_t end = line.find_first_of(delim, start);
                string word = line.substr(start, end - start);
                
                if (word.at(0) == '#')// starting a comment
                    break;
                
                size_t col_index;
                int source_min, target_min, target_max;
                switch (capture_flags){
                    case 0: // capturing the component name
                        // fetch component level from channel spec
                        for (int i = 0; i < int(T::Level::MAX); i++)
                            if (word.find(T::level_str[i]) != string::npos) {
                                level = i;
                                capture_flags ++;
                            }
                        break;

                    case 1: // capturing target bit(s)
                        col_index = word.find(":");
                        if ( col_index != string::npos ){
                            target_bit2 = stoi(word.substr(col_index+1));
                            word = word.substr(0,col_index);
                            is_range = true;
                        }
                        target_bit = stoi(word);
                        capture_flags ++;
                        break;

                    case 2: //this should be the delimiter
                        assert(word.find("=") != string::npos);
                        capture_flags ++;
                        break;

                    case 3:
                        if (is_range){
                            col_index = word.find(":");
                            source_bit  = stoi(word.substr(0,col_index));
                            source_bit2 = stoi(word.substr(col_index+1));
                            assert(source_bit2 - source_bit == target_bit2 - target_bit);
                            source_min = min(source_bit, source_bit2);
                            target_min = min(target_bit, target_bit2);
                            target_max = max(target_bit, target_bit2);
                            while (target_min <= target_max){
                                mapping_scheme[level][target_min].push_back(source_min);
                                // cout << target_min << " <- " << source_min << endl;
                                source_min ++;
                                target_min ++;
                            }
                        }
                        else {
                            source_bit = stoi(word);
                            mapping_scheme[level][target_bit].push_back(source_bit);
                        }
                }
                if (end == string::npos) { // this is the last word
                    break;
                }
                line = line.substr(end);
            }
        }
        if (dump_mapping)
            dump_mapping_scheme();
    }
    
    void dump_mapping_scheme(){
        cout << "Mapping Scheme: " << endl;
        for (MapScheme::iterator mapit = mapping_scheme.begin(); mapit != mapping_scheme.end(); mapit++)
        {
            int level = mapit->first;
            for (MapSchemeEntry::iterator entit = mapit->second.begin(); entit != mapit->second.end(); entit++){
                cout << T::level_str[level] << "[" << entit->first << "] := ";
                cout << "PhysicalAddress[" << *(entit->second.begin()) << "]";
                entit->second.erase(entit->second.begin());
                for (MapSrcVector::iterator it = entit->second.begin() ; it != entit->second.end(); it ++)
                    cout << " xor PhysicalAddress[" << *it << "]";
                cout << endl;
            }
        }
    }
    
    void apply_mapping(long addr, std::vector<int>& addr_vec){
        int *sz = spec->org_entry.count;
        int addr_total_bits = sizeof(addr_vec)*8;
        int addr_bits [int(T::Level::MAX)];
        for (int i = 0 ; i < int(T::Level::MAX) ; i ++)
        {
            if ( i != int(T::Level::Row))
            {
                addr_bits[i] = calc_log2(sz[i]);
                addr_total_bits -= addr_bits[i];
            }
        }
        // Row address is an integer.
        addr_bits[int(T::Level::Row)] = min((int)sizeof(int)*8, max(addr_total_bits, calc_log2(sz[int(T::Level::Row)])));

        // printf("Address: %lx => ",addr);
        for (unsigned int lvl = 0; lvl < int(T::Level::MAX); lvl++)
        {
            unsigned int lvl_bits = addr_bits[lvl];
            addr_vec[lvl] = 0;
            for (unsigned int bitindex = 0 ; bitindex < lvl_bits ; bitindex++){
                bool bitvalue = false;
                for (MapSrcVector::iterator it = mapping_scheme[lvl][bitindex].begin() ;
                    it != mapping_scheme[lvl][bitindex].end(); it ++)
                {
                    bitvalue = bitvalue xor get_bit_at(addr, *it);
                }
                addr_vec[lvl] |= (bitvalue << bitindex);
            }
            // printf("%s: %x, ",T::level_str[lvl].c_str(),addr_vec[lvl]);
        }
        // printf("\n");
    }

    int pending_requests()
    {
        int reqs = 0;
        for (auto ctrl: ctrls)
            reqs += ctrl->readq.size() + ctrl->writeq.size() + ctrl->otherq.size() + ctrl->actq.size() + ctrl->pending.size();
        return reqs;
    }

    void set_high_writeq_watermark(const float watermark) {
        for (auto ctrl: ctrls)
            ctrl->set_high_writeq_watermark(watermark);
    }

    void set_low_writeq_watermark(const float watermark) {
    for (auto ctrl: ctrls)
        ctrl->set_low_writeq_watermark(watermark);
    }

    void finish(void) {
      dram_capacity = max_address;
      int *sz = spec->org_entry.count;
      maximum_bandwidth = spec->speed_entry.rate * 1e6 * spec->channel_width * sz[int(T::Level::Channel)] / 8;
      long dram_cycles = num_dram_cycles.value();
      for (auto ctrl : ctrls) {
        long read_req = long(incoming_read_reqs_per_channel[ctrl->channel->id].value());
        ctrl->finish(read_req, dram_cycles);
      }

      // finalize average queueing requests
      in_queue_req_num_avg = in_queue_req_num_sum.value() / dram_cycles;
      in_queue_read_req_num_avg = in_queue_read_req_num_sum.value() / dram_cycles;
      in_queue_write_req_num_avg = in_queue_write_req_num_sum.value() / dram_cycles;
    }

    long page_allocator(long addr, int coreid, bool is_write, bool instr_limit) {
        if (instr_limit) {
            fprintf(stdout, "%f, ", (float)(float(dirty_write_physical_pages.value()) / total_dirty_pages.value()));
        }
        long virtual_page_number = addr >> 12;
        if (free_physical_pages_remaining != (long)physical_pages_remaining.value()) {
            physical_pages_remaining = free_physical_pages_remaining;
        }
        switch(int(translation)) {
            case int(Translation::None): {
              //fprintf(stdout, "came to find translatio: None\n");
              return addr;
            }
            case int(Translation::Random): {
                auto target = make_pair(coreid, virtual_page_number);
                //fprintf(stdout, "came to find translation\n");
                if(page_translation.find(target) == page_translation.end()) {
                    // page doesn't exist, so assign a new page
                    // make sure there are physical pages left to be assigned

                    // if physical page doesn't remain, replace a previous assigned
                    // physical page.
                    //fprintf(stdout, "couldnt find translation\n");
                    if (free_physical_pages_remaining <= 0) {
                      ////fprintf(stdout, "free_physical_pages_remaining %ld\n", (long)physical_pages_remaining.value());
                      physical_page_replacement++;
                      long phys_page_to_read = lrand() % free_physical_pages.size();
                      assert(free_physical_pages[phys_page_to_read] != -1);
                      /* if its not already captured */
                      
                      dirty_physical_pages.erase(phys_page_to_read);
                      
                      page_translation[target] = make_pair(phys_page_to_read, (int)is_write);
                      if (page_translation[target].second == 1) {
                        dirty_physical_pages.insert(phys_page_to_read);
                      } else {
                        dirty_physical_pages.erase(phys_page_to_read);
                      }

                    } else {
                        //fprintf(stdout, " assigning a new page\n");
                        // assign a new page
                        long phys_page_to_read = lrand() % free_physical_pages.size();
                        // if the randomly-selected page was already assigned
                        if(free_physical_pages[phys_page_to_read] != -1) {
                            long starting_page_of_search = phys_page_to_read;

                            do {
                                // iterate through the list until we find a free page
                                // TODO: does this introduce serious non-randomness?
                                ++phys_page_to_read;
                                phys_page_to_read %= free_physical_pages.size();
                            }
                            while((phys_page_to_read != starting_page_of_search) && free_physical_pages[phys_page_to_read] != -1);
                        }

                        assert(free_physical_pages[phys_page_to_read] == -1);

                        page_translation[target] = make_pair(phys_page_to_read,(int)is_write);
                        free_physical_pages[phys_page_to_read] = coreid;
                        if (is_write) {
                            dirty_physical_pages.insert(phys_page_to_read);
                        }
                        total_dirty_pages += 1;
                        --free_physical_pages_remaining;
                        --physical_pages_remaining;
                    }
                } else {
                    // /* Hack the code to reflect write physical pages */
                    if (is_write) {
                        /* if its not already captured */
                        if (page_translation[target].second != 1) {
                            page_translation[target].second = 1;
                            dirty_physical_pages.insert(page_translation[target].first);
                        }
                    } 
                }
                dirty_write_physical_pages = dirty_physical_pages.size();
                if (dirty_write_physical_pages.value() > max_dirty_pages.value()) {
                    max_dirty_pages = 0 ;
                    max_dirty_pages += dirty_write_physical_pages.value();
                }
                std::cout << "Log: page allocate: " << addr << " dirty_write_physical_pages: " << dirty_write_physical_pages.value() << "max_dirty_pages " << max_dirty_pages.value() << std::endl; 
                // } 
                // SAUGATA TODO: page size should not always be fixed to 4KB
                return (page_translation[target].first << 12) | (addr & ((1 << 12) - 1));
            }
            default:
                assert(false);
        }

    }

private:

    int calc_log2(int val){
        int n = 0;
        while ((val >>= 1))
            n ++;
        return n;
    }
    int slice_lower_bits(long& addr, int bits)
    {
        int lbits = addr & ((1<<bits) - 1);
        addr >>= bits;
        return lbits;
    }
    bool get_bit_at(long addr, int bit)
    {
        return (((addr >> bit) & 1) == 1);
    }
    void clear_lower_bits(long& addr, int bits)
    {
        addr >>= bits;
    }
    long lrand(void) {
        if(sizeof(int) < sizeof(long)) {
            return static_cast<long>(rand()) << (sizeof(int) * 8) | rand();
        }

        return rand();
    }
};

} /*namespace ramulator*/

#endif /*__MEMORY_H*/
