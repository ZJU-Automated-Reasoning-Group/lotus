#ifndef __FBVCLOCK_H__
#define __FBVCLOCK_H__
 
#include <string>
#include <vector>

#include <llvm/Support/raw_ostream.h>
 
 class FBVClock{
 public:
   typedef int ClockSystemID;
   static ClockSystemID new_clock_system();
   static void delete_clock_system(ClockSystemID cid);
   FBVClock(ClockSystemID cid, int idx);
   FBVClock(const FBVClock &) = default;
   bool operator[](int i);
   FBVClock &operator+=(FBVClock &c);
   std::string to_string() const;
   void invalidate() { cid = id = -1; }
   /* Returns some natural number i such that for all j s.t. i<=j, it
    * holds that (*this)[j] == false.
    *
    * In particular i will be the number of clock elements currently
    * kept track of by this clock.
    */
   int size() const { return sys[cid].idx_to_id.size(); }
 
 private:
   ClockSystemID cid;
   int id;
   int idx;
   struct ClockSystem{
     ClockSystem() : allocated(true), time(0) {}
     bool allocated;
     int time;
     std::vector<int> change_ts;
     std::vector<int> update_ts;
     std::vector<std::vector<bool> > clocks;
     std::vector<int> idx_to_id;
   };
   static std::vector<ClockSystem> sys;
 
   void update() const { update(cid,id); }
 
   static void update(ClockSystemID cid, int id);
   static void add_clock(std::vector<bool> &dst, std::vector<bool> &src);
 };
 
 inline std::ostream &operator<<(std::ostream &os, FBVClock &c){
   return os << c.to_string();
 }
 
 inline llvm::raw_ostream &operator<<(llvm::raw_ostream &os, FBVClock &c){
   return os << c.to_string();
 }
 
#endif
