//
// Created by ShiQingkai on 2021/7/24.
//

#ifndef CS_INDEXING_ABSTRACTQUERY_H
#define CS_INDEXING_ABSTRACTQUERY_H

/**
 * @brief Abstract base class for graph reachability queries.
 *
 * This class defines the interface for different reachability query algorithms
 * used in the CFL (Context-Free Language) indexing system.
 */
class AbstractQuery {
public:
  /**
   * @brief Check if there is a path from source to destination.
   * @param src Source vertex ID
   * @param dst Destination vertex ID
   * @return true if reachable, false otherwise
   */
  virtual bool reach(int src, int dst) = 0;

  /**
   * @brief Get the name of the query method.
   * @return C-string containing the method name
   */
  virtual const char *method() const = 0;

  /**
   * @brief Reset the query state.
   *
   * Clears any cached data or internal state to prepare for new queries.
   */
  virtual void reset() = 0;
};

#endif // CS_INDEXING_ABSTRACTQUERY_H
