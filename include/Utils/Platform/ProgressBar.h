
//===----------------------------------------------------------------------===//
/// @file ProgressBar.h
/// @brief Console progress bar display utility
///
/// This file provides a ProgressBar class for displaying progress in console
/// applications. It supports multiple visual styles and customizable update
/// frequencies.
///
/// @par Supported Styles:
/// - PBS_NumberStyle: Shows percentage as a number
/// - PBS_CharacterStyle: Shows progress using characters (e.g., "#####....")
/// - PBS_BGCStyle: Shows progress using background colors
///
///===----------------------------------------------------------------------===//

#ifndef SUPPORT_PROGRESSBAR_H
#define SUPPORT_PROGRESSBAR_H

#include <string>

/// @brief Console progress bar display class
///
/// This class provides functionality to display a progress bar in the console.
/// The progress bar can be updated incrementally as a long-running operation
/// progresses.
class ProgressBar {
public:
  /// @brief Enumeration of available progress bar styles
  enum ProgressBarStyle {
    PBS_NumberStyle,    ///< Show percentage as a number
    PBS_CharacterStyle, ///< Show progress using ASCII characters
    PBS_BGCStyle        ///< Show progress using background colors
  };

private:
  /// @brief Title of the progress bar
  std::string Title;

  /// @brief Style of the progress bar
  ProgressBarStyle Style;

  /// @brief Console window width
  unsigned WindowWidth;

  /// @brief The buffer of characters that forms the visual progress bar
  char *ProgressBuffer;

  /// @brief How frequently to update the progress bar
  /// @details For example, if Delta is 0.01, the bar updates every 1%
  float UpdateFrequency;
  float LastUpdatePercent = 0;

public:
  /// @brief Construct a progress bar
  /// @param title The title displayed before the progress bar
  /// @param style The visual style of the progress bar
  /// @param updateFrequency How often to update (default: 0.01 = 1%)
  ProgressBar(const std::string &Title, ProgressBarStyle Style,
              float UpdateFrequency = 0.01);

  /// @brief Destructor
  virtual ~ProgressBar();

  /// @brief Update the progress display
  /// @param percent Progress value in range [0, 1]
  /// @details 0.0 means just started, 1.0 means complete
  void showProgress(float percent);

  /// @brief Reset the progress bar to initial state
  void reset();

private:
  /// @brief Update the window width if console size changes at runtime
  void resize();
};

#endif // SUPPORT_PROGRESSBAR_H
