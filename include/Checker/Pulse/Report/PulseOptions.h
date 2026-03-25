#ifndef CHECKER_PULSE_PULSEOPTIONS_H
#define CHECKER_PULSE_PULSEOPTIONS_H

namespace pulse {

namespace options {

/** If true, SMT solving is disabled (fast mode): path conditions are not
 * checked with Z3; unsatisfiability is not queried. Fewer paths may be pruned.
 */
inline bool &disableSMTRef() {
  static bool value = false;
  return value;
}

inline bool disableSMT() { return disableSMTRef(); }
inline void setDisableSMT(bool b) { disableSMTRef() = b; }

} // namespace options
} // namespace pulse

#endif
