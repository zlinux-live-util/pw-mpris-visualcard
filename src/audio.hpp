#pragma once
// PipeWire audio capture: finds the audio stream node of one application and hands out its
// samples as mono float blocks.
//
// "Raw" is the whole point. The stream is taken off the player's own monitor port, so it is the
// signal that player submitted, ahead of any device mixer, volume stage or effect. Nothing is
// added on the way in: no gain, no gate, no resampling to a fixed rate. Blocks arrive at the
// graph's own rate and the analyser works in that rate (see analyser.hpp).
//
// Capture runs on its own PipeWire thread loop. The realtime side only mixes the channels of one
// buffer down to mono and writes it into a preallocated ring; resolving a node and connecting a
// stream happen on a timer on the loop thread, never in the audio callback.
#include <cstddef>
#include <memory>
#include <string>

namespace oms {

class AudioTap {
 public:
  /** ringSeconds: how much history the ring keeps. One second is plenty; it only has to span the
   *  time between two rendered frames. */
  explicit AudioTap(int ringSeconds, bool verbose);
  ~AudioTap();
  AudioTap(const AudioTap&) = delete;
  AudioTap& operator=(const AudioTap&) = delete;

  /** Connects to PipeWire and starts the capture loop. Throws std::runtime_error on failure. */
  void start();
  void stop();

  /** What to capture: an MPRIS player name, a node name or an application id, matched against
   *  the live registry. Empty suspends capture. Callable from any thread; the resolution itself
   *  happens on a timer on the loop thread, so this never blocks the caller. */
  void setTarget(const std::string& name);

  /** Whether capture is wanted. While false the stream stays disconnected, which keeps a card
   *  with no consumer attached at zero cost. */
  void setActive(bool active);

  /** Graph sample rate the samples arrive at, 0 while disconnected. Lock-free, so it is safe to
   *  call once per frame. */
  int rate() const;

  /** Copies up to n of the most recent mono samples, oldest first, and returns how many were
   *  available. Never blocks: a caller that falls behind loses the oldest samples instead of
   *  accumulating latency. */
  size_t read(float* out, size_t n);

  /** Capture state, for logging. Not on the per-frame path: it takes a lock. */
  struct Status {
    bool active = false;     // setActive(true)
    bool connected = false;  // the input stream is streaming
    std::string target;      // what setTarget() asked for
    std::string node;        // resolved node.name, empty while unresolved
    int rate = 0;            // graph rate the samples arrive at
    int channels = 0;
  };
  Status status() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace oms
