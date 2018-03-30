#ifndef RRC_H
#define RRC_H

#include "RRC_MainWindow_UI.h"

#include <rt.h>
#include <settings.h>
#include <workspace.h>
#include <event.h>
#include <plugin.h>

#include <QtGlobal>
#include <QtWidgets>

namespace RRC {
class Module: public QWidget, public RT::Thread, public Plugin::Object,
              public Workspace::Instance, public Event::Handler,
              public Event::RTHandler {
  Q_OBJECT // Required macro for QT slots

 public:
  Module();
  ~Module();
  void execute(); // Function run at every RTXI loop
  void receiveEvent(const ::Event::Object *); // Receive non-RT event
  void receiveEventRT(const ::Event::Object *); // Receive RT event

 public slots:
  void refreshDisplay(); // Refresh user interface
  void modify(); // Update parameters
  void toggle_stimThreshold(); // Called when stim threshold button is pressed
  void toggle_pace(); // Called when pace button is pressed
  void toggle_rrcThreshold(); // Called when RRC threshold button is pressed
  void toggle_rrcProtocol(); // Called when RRC protocol button is pressed

 private:
  // Ui elements
  QWidget *rrcWindow;
  QMdiSubWindow *subWindow;
  Ui::RRC_UI rrcUi;

  // Module functions
  void createGUI();
  void initialize();
  void reset();
  void dataRecord_start();
  void dataRecord_stop();

  // Workspace variables
  //// States
  double time; // Time elapsed during protocol (ms)
  double voltage; // Membrane voltage of cell (mV)
  double beatNumber; // Beats elapsed during protocol
  double apd; // Action potential duration (ms)
  // Parameters
  //// Stimulus tab
  double bcl; // Basic cycle length (ms)
  double stim_amplitude; // Stimulus amplitude (nA)
  double stim_length; // Stimulus length (ms)
  double ljp; // Liquid junction potential (mV)
  double cm; // Membrane capacitance (pF)
  //// RRC threshold tab
  double thresh_startAmplitude; // Start amplitude for RRC threshold test (nA)
  double thresh_ampIncrement; // Increment amplitude of RRC threshold test (nA)
  int thresh_beatNumber; // Number of beats before each RRC injection
  int thresh_apdCutoff; // APD change that denotes end of RRC threshold test
  //// RRC protocol tab
  double rrc_amplitude; // Amplitude of repolarization reserve current (nA)
  double rrc_delay; // Delay before the start of RRC injection (ms)
  int rrc_length; // Length of RRC, where 0 indicates until next stimulus
  int rrc_thresholdWindow; // Change in amplitude for sub- and supra-threshold
  int rrc_beatNumber; // Number of beats before each RRC injection
  int rrc_chance; // Random chance for either a sub- or supra-threshold RRC
  int rrc_endBeatNumber; // Number of total beats for RRC injection protocol
  //// APD tab
  int apd_repolPercent; // Action potential duration repolarization percentage
  int apd_min; // Minimum duration of depolarization that counts as AP (ms)
  int apd_stimWindow; // Window of time after stimulus ignored

  // Int conversions to prevent rounding errors;
  int time_int;
  int bcl_int;
  int stim_length_int;
  // Beat number must be double in order to be a workspace state
  int beatNumber_int;

  // Execute variables
  double outputCurrent;
  double period; // RTXI thread period
  enum execute_mode_t {IDLE, STIMTHRESHOLD, PACE, RRCTHRESHOLD, RRCPROTOCOL}
    execute_mode;
  bool recording; // Flag to denote if data recorder is recording
  //// Pace
  bool pace_onFlag; // Flag to denote state of pace button
  bool pace_recordData; // Flag to denote if data will be recorded
  int bcl_startTime; // Start time tracker for basic cycle length
  int bcl_stepTime;
  //// Stimulus Threshold
  bool stim_onFlag; // Flag to denote state of stimulus threshold button
  bool stim_recordData; // Flag to denote if data will be recorded
  bool stim_backToBaseline;
  double stim_peakVoltage;
  double stim_vmRest;
  double stim_responseDuration;
  double stim_responseTime;
  double stim_startTime;
  double stim_stimulusLevel;
  //// RRC Threshold
  bool thresh_onFlag; // Flag to denote state of RRC threshold button
  bool thresh_recordData; // Flag to denote if data will be recorded
  bool thresh_rrcThreshFound; // Flag to denote if search has completed
  double thresh_previousAPD; // Holder for APD during a RRC injection
  double thresh_rrcAmplitude;
  //// RRC Protocol
  bool rrcProtocol_onFlag; // Flag to denote state of pace button
  bool rrcProtocol_recordData; // Flag to denote if data will be recorded
  int rrc_startTime; // Start time for RRC injection
  int rrc_endTime; // End time for RRC injection
  int rrc_random_injection;
  int rrc_random_threshold;

  // APD calculation
  void calculateAPD(int);
  enum apd_mode_t {START, PEAK, DOWN, DONE} apd_mode;
  double apd_vmRest; // Resting membrane potential, i.e. Vm prior to stimulus
  double apd_upstrokeThreshold; // Upstroke threshold for start of AP
  double apd_downstrokeThreshold; // Downstroke threshold for end of AP
  double apd_startTime; // Time the action potential starts
  double apd_peakTime; // Time of action potential peak
  double apd_peakVoltage; // Voltage of action potential peak
  double apd_endTime; // Time of action potential end

 protected:
  void doLoad(const Settings::Object::State &);
  void doSave(Settings::Object::State &) const;
}; // Class Module
}; // Namespace RRC

#endif // RRC_H
