#include "Utils/Platform/ProgressBar.h"

#include <cassert>
#include <cstring>

#include <sys/ioctl.h>
#include <unistd.h>

// Constructs a progress bar with the given title, style, and update frequency.
ProgressBar::ProgressBar(const std::string &Title, ProgressBarStyle Style,
                         float UpdateFrequency)
    : Title(Title), Style(Style), UpdateFrequency(UpdateFrequency) {

  struct winsize WinSize{};
  ioctl(STDIN_FILENO, TIOCGWINSZ, &WinSize);
  if (WinSize.ws_col > 80) {
    WindowWidth = 80;
  } else if (WinSize.ws_col >= Title.length() + 15) {
    WindowWidth = WinSize.ws_col - Title.length() - 15;
  } else {
    WindowWidth = 0;
  }

  if (WindowWidth < 15) {
    ProgressBuffer = nullptr;
    return;
  }

  ProgressBuffer = (char *)malloc(sizeof(char) * (WindowWidth + 1));
  if (Style == PBS_BGCStyle) {
    memset(ProgressBuffer, 0x00, WindowWidth + 1);
  } else {
    memset(ProgressBuffer, 32, WindowWidth);
    memset(ProgressBuffer + WindowWidth, 0x00, 1);
  }
}

// Displays the current progress percentage.
void ProgressBar::showProgress(float Percent) {
  if (Percent < 0)
    Percent = 0;
  else if (Percent > 1)
    Percent = 1;

  if (Percent > 0 && Percent < 1) {
    if (Percent - LastUpdatePercent < UpdateFrequency) {
      return;
    }
  }

  LastUpdatePercent = Percent;

  // In case the window width is changed at runtime.
  resize();

  auto Val = (unsigned)(Percent * (float)WindowWidth);
  switch (this->Style) {
  case PBS_NumberStyle:
    printf("\033[?25l\033[37m\033[1m%s %d%%\033[?25h\033[0m\r", Title.c_str(),
           (int)(Percent * 100));
    break;
  case PBS_CharacterStyle:
    if (ProgressBuffer) {
      memset(ProgressBuffer, '#', Val);
      printf("\033[?25l\033[37m\033[1m%s [%-s] %d%%\033[?25h\033[0m\r",
             Title.c_str(), ProgressBuffer, (int)(Percent * 100));
    } else {
      printf("\033[?25l\033[37m\033[1m%s %d%%\033[?25h\033[0m\r", Title.c_str(),
             (int)(Percent * 100));
    }
    break;
  case PBS_BGCStyle:
    if (ProgressBuffer) {
      memset(ProgressBuffer, 32, Val);
      printf("\033[?25l\033[31m\033[1m%s \033[47m %d%% %s\033[?25h\033[0m\r",
             Title.c_str(), (int)(Percent * 100), ProgressBuffer);
    } else {
      printf("\033[?25l\033[37m\033[1m%s %d%%\033[?25h\033[0m\r", Title.c_str(),
             (int)(Percent * 100));
    }
    break;
  default:
    assert(false && "Unknown style!");
  }
  fflush(stdout);
}

// Resizes the progress bar based on current terminal width.
void ProgressBar::resize() {
  // clear the line.
  printf("\r\033[K");

  unsigned CurrentWindowWidth;
  struct winsize WinSize{};
  ioctl(STDIN_FILENO, TIOCGWINSZ, &WinSize);
  if (WinSize.ws_col > 80) {
    CurrentWindowWidth = 80;
  } else if (WinSize.ws_col >= Title.length() + 15) {
    CurrentWindowWidth = WinSize.ws_col - Title.length() - 15;
  } else {
    CurrentWindowWidth = 0;
  }

  if (CurrentWindowWidth < 15) {
    free(ProgressBuffer);
    ProgressBuffer = nullptr;
    return;
  }

  if (WindowWidth == CurrentWindowWidth) {
    return;
  }

  WindowWidth = CurrentWindowWidth;
  ProgressBuffer =
      (char *)realloc(ProgressBuffer, sizeof(char) * (WindowWidth + 1));
  if (Style == PBS_BGCStyle) {
    memset(ProgressBuffer, 0x00, WindowWidth + 1);
  } else {
    memset(ProgressBuffer, 32, WindowWidth);
    memset(ProgressBuffer + WindowWidth, 0x00, 1);
  }
}

// Resets the progress bar to 0%.
void ProgressBar::reset() {
  LastUpdatePercent = 0;
  if (ProgressBuffer) {
    if (Style == PBS_BGCStyle) {
      memset(ProgressBuffer, 0x00, WindowWidth + 1);
    } else {
      memset(ProgressBuffer, 32, WindowWidth);
      memset(ProgressBuffer + WindowWidth, 0x00, 1);
    }
  }
}

// Destructor cleans up the progress buffer.
ProgressBar::~ProgressBar() {
  if (ProgressBuffer) {
    free(ProgressBuffer);
    ProgressBuffer = nullptr;
  }
}
