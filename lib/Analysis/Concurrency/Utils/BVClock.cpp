/*
 *
 * Author: rainoftime
*/
#include "Analysis/Concurrency/Utils/BVClock.h"
#include "Analysis/Concurrency/Utils/FBVClock.h"

#include <algorithm>
#include <string>

BVClock BVClock::operator+(const BVClock &vc) const{
  BVClock rv;
  if(vec.size() < vc.vec.size()){
    rv.vec = vc.vec;
  }else{
    rv.vec = vec;
  }
  const unsigned m = std::min(vc.vec.size(),vec.size());
  for(unsigned i = 0; i < m; ++i){
    rv.vec[i] = vec[i] || vc.vec[i];
  }
  return rv;
}

std::string BVClock::to_string() const{
  if(vec.size()){
    std::string s;
    s.resize(2*vec.size()+1,',');
    s[0] = '[';
    s[s.size()-1] = ']';
    for(unsigned i = 0; i < vec.size(); ++i){
      s[2*i+1] = vec[i] ? '1' : '0';
    }
    return s;
  }
  return "[]";
}

bool BVClock::leq(const BVClock &vc) const{
  const int m = std::min(vec.size(),vc.vec.size());
  for(int i = 0; i < m; ++i){
    if(vec[i] && !vc.vec[i]) return false;
  }
  for(unsigned i = vc.vec.size(); i < vec.size(); ++i){
    if(vec[i]) return false;
  }
  return true;
}

bool BVClock::lt(const BVClock &vc) const{
  return leq(vc) && *this != vc;
}

bool BVClock::gt(const BVClock &vc) const{
  return vc.lt(*this);
}

bool BVClock::geq(const BVClock &vc) const{
  return vc.leq(*this);
}

bool BVClock::operator==(const BVClock &vc) const{
  return leq(vc) && vc.leq(*this);
}

bool BVClock::operator!=(const BVClock &vc) const{
  return !(*this == vc);
}

bool BVClock::operator<(const BVClock &vc) const{
  return lt(vc);
}

bool BVClock::operator<=(const BVClock &vc) const{
  return leq(vc);
}

bool BVClock::operator>(const BVClock &vc) const{
  return gt(vc);
}

bool BVClock::operator>=(const BVClock &vc) const{
  return geq(vc);
}

BVClock &BVClock::operator=(FBVClock &vc){
  const int sz = vc.size();
  vec.resize(sz,false);
  for(int i = 0; i < sz; ++i){
    vec[i] = vc[i];
  }
  return *this;
}

BVClock &BVClock::operator+=(FBVClock &vc){
  const unsigned sz = vc.size();
  if(vec.size() < sz){
    vec.resize(sz,false);
  }
  for(unsigned i = 0; i < sz; ++i){
    vec[i] = vec[i] || vc[i];
  }
  return *this;
}
