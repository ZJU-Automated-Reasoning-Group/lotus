//===----------------------------------------------------------------------===//
/// @file RNG.h
/// @brief Random number generator based on Mersenne Twister
///
/// This file provides a random number generator implementation based on the
/// Mersenne Twister algorithm (MT19937). It generates high-quality random
/// numbers suitable for scientific and simulation applications.
///
/// The implementation is based on the work by Makoto Matsumoto and provides
/// a period of 2^19937-1.
///
///===----------------------------------------------------------------------===//

/*
 * @author rainoftime
 */

#ifndef UTIL_RNG_H
#define UTIL_RNG_H

/// @brief Mersenne Twister random number generator
///
/// This class implements the MT19937 Mersenne Twister algorithm for generating
/// high-quality pseudo-random numbers. It provides methods for generating
/// random integers and floating-point numbers in various ranges.
class RNG {
private:
  /// Period parameters
  static const int N = 624;                          ///< State array size
  static const int M = 397;                          ///< Middle word offset
  static const unsigned int MATRIX_A = 0x9908b0dfUL; ///< Constant vector a
  static const unsigned int UPPER_MASK =
      0x80000000UL; ///< Most significant w-r bits
  static const unsigned int LOWER_MASK =
      0x7fffffffUL; ///< Least significant r bits

private:
  unsigned int mt[N]; ///< The state vector
  int mti;            ///< Current position in state vector

public:
  /// @brief Construct a random number generator with a seed
  /// @param seed The initial seed value (default: 5489)
  RNG(unsigned int seed = 5489UL);

  /// @brief Re-seed the random number generator
  /// @param seed The new seed value
  void seed(unsigned int seed);

  /// @brief Generate a random 32-bit unsigned integer
  /// @return Random value in range [0, 0xffffffff]
  unsigned int getInt32();

  /// @brief Generate a random positive 31-bit integer
  /// @return Random value in range [0, 0x7fffffff]
  int getInt31();

  /// @brief Generate a random double in [0, 1] (inclusive)
  /// @return Random value in range [0, 1]
  double getDoubleLR();

  /// @brief Generate a random float in [0, 1] (inclusive)
  /// @return Random value in range [0, 1]
  float getFloatLR();

  /// @brief Generate a random double in [0, 1) (half-open)
  /// @return Random value in range [0, 1)
  double getDoubleL();

  /// @brief Generate a random float in [0, 1) (half-open)
  /// @return Random value in range [0, 1)
  float getFloatL();

  /// @brief Generate a random double in (0, 1) (exclusive)
  /// @return Random value in range (0, 1)
  double getDouble();

  /// @brief Generate a random float in (0, 1) (exclusive)
  /// @return Random value in range (0, 1)
  float getFloat();

  /// @brief Generate a random boolean
  /// @return Random true or false with equal probability
  bool getBool();
};

#endif
