#pragma once

#include <iterator>
#include <ostream>

namespace util {
namespace io {

template <typename T, typename OStreamType>
class InfixOstreamIterator
    : public std::iterator<std::output_iterator_tag, void, void, void, void> {
private:
  OStreamType &os;
  const char *delimiter;
  bool isFirstElem;

public:
  InfixOstreamIterator(OStreamType &s)
      : os(s), delimiter(0), isFirstElem(true) {}
  InfixOstreamIterator(OStreamType &s, const char *d)
      : os(s), delimiter(d), isFirstElem(true) {}

  InfixOstreamIterator &operator=(const T &item) {
    if (delimiter != 0 && !isFirstElem)
      os << delimiter;
    os << item;
    isFirstElem = false;
    return *this;
  }

  InfixOstreamIterator &operator*() { return *this; }
  InfixOstreamIterator &operator++() { return *this; }
  InfixOstreamIterator &operator++(int) { return *this; }
};

template <typename T, typename OStreamType>
InfixOstreamIterator<T, OStreamType> infix_ostream_iterator(OStreamType &s,
                                                            const char *d) {
  return InfixOstreamIterator<T, OStreamType>(s, d);
}

} // namespace io
} // namespace util