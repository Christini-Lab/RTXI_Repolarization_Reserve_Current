#include "RRC.h"

#include <iostream>
#include <cmath>
#include <cstdlib>

#include <main_window.h>
#include <data_recorder.h>

#include <QtGlobal>
#include <QtWidgets>

namespace {
class RRC_SyncEvent : public RT::Event {
 public:
  int callback() {
    return 0;
  }
};
}

// Create Module Instance
extern "C" Plugin::Object *createRTXIPlugin() {
  return new RRC::Module();
}

// Workspace
static Workspace::variable_t vars[] = {
  // Inputs
  { "Input Voltage (V)",
    "Input voltage (V) from target cell",
    Workspace::INPUT, },
  // Outputs
  { "Output Current (A)",
    "Output current (A) to target cell or internal input",
    Workspace::OUTPUT, },
  // States
  { "Time (ms)",
    "Time Elapsed (ms)",
    Workspace::STATE, },
  { "Voltage (mV)",
    "Membrane voltage (mV) of target cell computed from amplifier input",
    Workspace::STATE, },
  { "Beat Number",
    "Number of beats",
    Workspace::STATE, },
  { "APD (ms)",
    "Action potential duration of cell (ms)",
    Workspace::STATE, },
  // Stimulus Parameters
  { "Stimulus Window (ms)",
    "Window of time after stimulus that is ignored by APD calculation",
    Workspace::PARAMETER, },
  { "Stimulus Amplitude (nA)",
    "Amplitude of stimulation pulse (nA)",
    Workspace::PARAMETER, },
  { "Stimulus Length (ms)",
    "Duration of stimulation pulse (nA)",
    Workspace::PARAMETER, },
  { "Cm (pF)",
    "Membrane capacitance of cell (pF)",
    Workspace::PARAMETER, },
  { "LJP (mv)",
    "Liquid junction potential (mV)",
    Workspace::PARAMETER, },
  // RRC Threshold Parameters
  { "Threshold Start Amplitude (nA)",
    "Starting amplitude for RRC threshold test (nA)",
    Workspace::PARAMETER, },
  { "Threshold Amplitude Increment (nA)",
    "Increment amplitude of RRC threshold test (nA)",
    Workspace::PARAMETER, },
  { "Threshold Beat Number",
    "Number of beats before each RRC injection",
    Workspace::PARAMETER, },
  { "Threshold APD Change Cutoff (%)",
    "APD change that denotes end of RRC threshold test (delta APD %)",
    Workspace::PARAMETER, },
  // RRC Protocol Parameters
  { "RRC Amplitude (ms)",
    "Amplitude of RRC",
    Workspace::PARAMETER, },
  { "RRC Delay (ms)",
    "Delay after stimulus denoting the start of RRC injection",
    Workspace::PARAMETER, },
  { "RRC Length (ms)",
    "Length of RRC injection. 0 indicates continuation until next stimulus",
    Workspace::PARAMETER, },
  { "RRC Threshold Window (%)",
    "Change in amplitude for sub- and supra-threshold RRC injections",
    Workspace::PARAMETER, },
  { "RRC Beat Number",
    "Number of beats before each RRC injection",
    Workspace::PARAMETER, },
  { "RRC Chance (%)",
    "Random chance for either a sub- or supra-threshold RRC injection",
    Workspace::PARAMETER, },
  { "RRC End Beat Number",
    "Number of total beats for RRC injection protocol",
    Workspace::PARAMETER, },
  // APD Parameters
  { "APD Repolarization %",
    "Percentage of repolarization that denotes end of action potental",
    Workspace::PARAMETER, },
  { "Minimum APD (ms)",
    "Minimum depolarization duration considered to be an action potential (ms)",
    Workspace::PARAMETER, },
};

// Number of variables in vars
static size_t num_vars = sizeof(vars) / sizeof(Workspace::variable_t);

RRC::Module::Module() :
    QWidget(MainWindow::getInstance()->centralWidget()),RT::Thread(0),
    Workspace::Instance("Repolarization Reserve Current Module",
                        vars, num_vars) {

  // Build module GUI
  setWindowTitle(QString::number(getID()) +
                 " Repolarization Reserve Current Module");
  createGUI();

  // Initialize parameters, initialize states, reset model, and update rate
  initialize();

  refreshDisplay();
  show();
}

RRC::Module::~Module() {
  // Make sure real-time thread is not in the middle of execution
  setActive(false);
  RRC_SyncEvent event;
  RT::System::getInstance()->postEvent(&event);
}

void RRC::Module::execute() {
  voltage = input(0) * 1e3 - ljp;

  switch(execute_mode) {
    case IDLE:
      break;

    case PACE: // Static pacing
      time += period;
      time_int += 1;

      if (time_int == 0 && pace_recordData && !recording)
        dataRecord_start();

      // If time is greater than BCL, advance the beat
      if (time_int - bcl_startTime >= bcl_int) {
        beatNumber++;
        bcl_startTime = time_int;
        apd_vmRest = voltage;
        // If AP has not ended before new stimulus, do not restart APD
        // calculation
        if (apd_mode != DOWN)
          // First step is APD calculate called at each stimulus
          calculateAPD(1);
      }

      // Stimulate cell for denoted stimulation length
      if ((time_int - bcl_startTime) < stim_length_int) {
        // Stimulus amplitude in nA, convert to A for amplifier
        outputCurrent = stim_amplitude * 1e-9;
      }
      else
        outputCurrent = 0;

      // Set module output
      output(0) = outputCurrent;

      // Calculate APD
      calculateAPD(2); // Second step of APD calculation
      break;

    case STIMTHRESHOLD: // Stimulus threshold search
      time += period;
      time_int += 1;

      if (time_int == 0 && stim_recordData && !recording)
        dataRecord_start();

      // Apply stimulus for given number of ms (StimLength)
      if (time_int - bcl_startTime < stim_length_int) {
        stim_backToBaseline = false;

        // stimulsLevel is in nA, convert to A for amplifier
        output(0) = stim_stimulusLevel * 1e-9;
      }
      else {
        output(0) = 0;

        // Find peak voltage after stimulus
        if (voltage > stim_peakVoltage)
          stim_peakVoltage = voltage;

        // If Vm is back to resting membrane potential (within 2 mV;
        // determined when threshold detection button is first pressed)
        // Vrest: voltage at the time threshold test starts
        if (voltage - stim_vmRest < 2) {
          if (!stim_backToBaseline) {
            stim_responseDuration = time - stim_startTime;
            stim_responseTime = time;
            stim_backToBaseline = true;
          }

          // Calculate time length of voltage response
          // If the response was more than 50ms long and peakVoltage is
          // more than 10mV, consider it an action potential
          if (stim_responseDuration > 50 && stim_peakVoltage > 10) {
            // Set the current stimulus value as 1.25x calculated threshold
            stim_amplitude = stim_stimulusLevel * 1.25;
            stim_onFlag = false;
            execute_mode = IDLE;

            if (recording)
              dataRecord_stop();
          }
          else { // If no action potential occurred, and Vm is back to rest
            // If the cell has rested for  200ms since returning to baseline
            if (time - stim_responseTime > 200) {
              // Increase the magnitude of the stimulus and try again
              stim_stimulusLevel += 0.1;

              // Record the time of stimulus application
              stim_startTime = time;
              bcl_startTime = time_int;
            }
          }
        }
      }
      break;

    case RRCTHRESHOLD: // repolarization reserve current threshold search
      time += period;
      time_int += 1;

      if (time_int == 0 && thresh_recordData && !recording)
        dataRecord_start();

      // If time is greater than BCL, advance the beat
      if (time_int - bcl_startTime >= bcl_int) {
        // Compare APDs between previous RRC injection to see if it passes
        // APD cutoff, if so, end threshold test
        if (beatNumber_int % thresh_beatNumber == 0) {
          if (thresh_previousAPD < 0) // Less than 0 before first RRC injection
            thresh_previousAPD = apd;
          // If cell has not repolarized prior to stim, end search
          else if (apd_mode == DOWN)
            thresh_rrcThreshFound = true;
          // Check if RRC injection APD passes cutoff based on previous APD
          else if (apd >= thresh_previousAPD * (1 + (thresh_apdCutoff / 100.0)))
            thresh_rrcThreshFound = true;
          else { // Continue search, increase RRC amplitude
            thresh_previousAPD = apd;
            thresh_rrcAmplitude += thresh_ampIncrement;
          }
        }

        if (thresh_rrcThreshFound) {
          execute_mode = IDLE;
          thresh_onFlag = false;
          output(0) = 0;

          if (recording)
            dataRecord_stop();
          break;
        }

        beatNumber++;
        beatNumber_int++;
        bcl_startTime = time_int;
        apd_vmRest = voltage;

        // If AP has not ended before new stimulus, do not restart APD
        // calculation
        if (apd_mode != DOWN)
          // First step in APD calculate called at each stimulus
          calculateAPD(1);

        // Set start and end time for RRC injection
        rrc_startTime = stim_length_int + (rrc_delay / period);
        // If length is set to 0, RRC continues until next stimulus
        if (rrc_length == 0)
          rrc_endTime = bcl_int;
        else
          rrc_endTime = rrc_length / period; // Convert to unitless
      }

      outputCurrent = 0;
      // Stimulate cell for denoted stimulation length
      if ((time_int - bcl_startTime) < stim_length_int) {
        // Stimulus amplitude in nA, convert to A for amplifier
        outputCurrent += stim_amplitude * 1e-9;
      }
      // Perform RRC injection every rrc_beatNumber beats
      if (beatNumber_int % thresh_beatNumber == 0) {
        if ((time_int - bcl_startTime) > rrc_startTime &&
            (time_int - bcl_startTime) < rrc_endTime)
          outputCurrent += thresh_rrcAmplitude * 1e-9;
      }
      // Set module output
      output(0) = outputCurrent;

      // Calculate APD
      calculateAPD(2); // Second step of APD calculation
      break;

    case RRCPROTOCOL: // Random repolarization reserve current injection
      time += period;
      time_int += 1;

      if (time_int == 0 && rrcProtocol_recordData && !recording)
        dataRecord_start();

      // If time is greater than BCL, advance the beat
      if (time_int - bcl_startTime >= bcl_int) {
        if (beatNumber >= rrc_endBeatNumber) { // End of protocol
          if (recording) {
            dataRecord_stop();
          }

          rrcProtocol_onFlag = false;
          execute_mode = IDLE;
          output(0) = 0;
          break;
        }

        beatNumber++;
        beatNumber_int++;
        bcl_startTime = time_int;
        apd_vmRest = voltage;
        // If AP has not ended before new stimulus, do not restart APD
        // calculation
        if (apd_mode != DOWN)
          // First step is APD calculate called at each stimulus
          calculateAPD(1);

        // Used to determine whether RRC injection will be performed
        // Random number between 1 and 100
        rrc_random_injection = std::rand() % 100 + 1;
        // Used to determine if injection is sub- or supra- threshold
        rrc_random_threshold = std::rand() % 100 + 1;
        // Set start and end time for RRC injection
        rrc_startTime = stim_length_int + (rrc_delay / period);
        // If length is set to 0, RRC continues until next stimulus
        if (rrc_length == 0)
          rrc_endTime = bcl_int;
        else
          rrc_endTime = rrc_length / period; // Convert to unitless
      }

      outputCurrent = 0;
      // Stimulate cell for denoted stimulation length
      if ((time_int - bcl_startTime) < stim_length_int) {
        // Stimulus amplitude in nA, convert to A for amplifier
        outputCurrent += stim_amplitude * 1e-9;
      }
      // Perform RRC injection every rrc_beatNumber beats and if random
      // number is greater than rrc_chance
      if (beatNumber_int % rrc_beatNumber == 0 &&
          rrc_random_injection <= rrc_chance) {
        if ((time_int - bcl_startTime) > rrc_startTime &&
            (time_int - bcl_startTime) < rrc_endTime) {
          if (rrc_random_threshold >= 50)
            outputCurrent +=
                rrc_amplitude * (1 + (rrc_thresholdWindow / 100.0)) * 1e-9;
          else
            outputCurrent +=
                rrc_amplitude * (1 - (rrc_thresholdWindow / 100.0)) * 1e-9;
        }
      }
      // Set module output
      output(0) = outputCurrent;

      // Calculate APD
      calculateAPD(2); // Second step of APD calculation
      break;
  }
}

void RRC::Module::createGUI() {
  // Create subwindow and add it to main RTXI window
  subWindow = new QMdiSubWindow(MainWindow::getInstance());
  subWindow->setAttribute(Qt::WA_DeleteOnClose);
  subWindow->setWindowIcon(QIcon("/usr/local/lib/rtxi/RTXI-widget-icon.png"));
  subWindow->setWindowFlags(Qt::CustomizeWindowHint |
                            Qt::WindowCloseButtonHint |
                            Qt::WindowMinimizeButtonHint );
  MainWindow::getInstance()->createMdi(subWindow);
  // Set this widget to newly created subwindow
  subWindow->setWidget(this);

  // Initialize Qt designer derived widget
  rrcWindow = new QWidget(this);
  rrcUi.setupUi(rrcWindow);

  // Add newly created widget to layout of this widget
  QVBoxLayout *layout = new QVBoxLayout(this);
  setLayout(layout);
  layout->addWidget(rrcWindow);

  // Set Ui refresh rate
  QTimer *timer = new QTimer(this);
  timer->start(100); // 100ms refresh rate

  // Set validators for edit widgets
  // Stimulus tab
  rrcUi.bcl_edit->setValidator(new QDoubleValidator(this));
  rrcUi.stim_amplitude_edit->setValidator(new QDoubleValidator(this));
  rrcUi.stim_length_edit->setValidator(new QDoubleValidator(this));
  rrcUi.ljp_edit->setValidator(new QDoubleValidator(this));
  rrcUi.cm_edit->setValidator(new QDoubleValidator(this));
  // RRC threshold tab
  rrcUi.thresh_startAmplitude_edit->setValidator(new QDoubleValidator(this));
  rrcUi.thresh_ampIncrement_edit->setValidator(new QDoubleValidator(this));
  rrcUi.thresh_beatNumber_edit->setValidator(new QIntValidator(this));
  rrcUi.thresh_apdCutoff_edit->setValidator(new QIntValidator(this));
  // RRC protocol tab
  rrcUi.rrc_amplitude_edit->setValidator(new QDoubleValidator(this));
  rrcUi.rrc_delay_edit->setValidator(new QDoubleValidator(this));
  rrcUi.rrc_length_edit->setValidator(new QIntValidator(this));
  rrcUi.rrc_thresholdWindow_edit->setValidator(new QIntValidator(this));
  rrcUi.rrc_beatNumber_edit->setValidator(new QIntValidator(this));
  rrcUi.rrc_chance_edit->setValidator(new QIntValidator(this));
  rrcUi.rrc_endBeatNumber_edit->setValidator(new QIntValidator(this));
  // APD tab
  rrcUi.apd_repolPercent_edit->setValidator(new QIntValidator(this));
  rrcUi.apd_min_edit->setValidator(new QIntValidator(this));
  rrcUi.apd_stimWindow_edit->setValidator(new QIntValidator(this));

  // Connect rrcUi elements to slot functions
  // Buttons box
  QObject::connect(rrcUi.stimThreshold_button, SIGNAL(clicked()),
                   this, SLOT(toggle_stimThreshold()));
  QObject::connect(rrcUi.pace_button, SIGNAL(clicked()),
                   this, SLOT(toggle_pace()));
  QObject::connect(rrcUi.rrcThreshold_button, SIGNAL(clicked()),
                   this, SLOT(toggle_rrcThreshold()));
  QObject::connect(rrcUi.rrcProtocol_button, SIGNAL(clicked()),
                   this, SLOT(toggle_rrcProtocol()));
  // RRC threshold tab
  QObject::connect(rrcUi.thresh_startAmplitude_edit, SIGNAL(returnPressed()),
                   this, SLOT(modify()));
  QObject::connect(rrcUi.thresh_ampIncrement_edit, SIGNAL(returnPressed()),
                   this, SLOT(modify()));
  QObject::connect(rrcUi.thresh_beatNumber_edit, SIGNAL(returnPressed()),
                   this, SLOT(modify()));
  QObject::connect(rrcUi.thresh_apdCutoff_edit, SIGNAL(returnPressed()),
                   this, SLOT(modify()));
  // Stimulus tab
  QObject::connect(rrcUi.bcl_edit, SIGNAL(returnPressed()),
                   this, SLOT(modify()));
  QObject::connect(rrcUi.stim_amplitude_edit, SIGNAL(returnPressed()),
                   this, SLOT(modify()));
  QObject::connect(rrcUi.stim_length_edit, SIGNAL(returnPressed()),
                   this, SLOT(modify()));
  QObject::connect(rrcUi.ljp_edit, SIGNAL(returnPressed()),
                   this, SLOT(modify()));
  QObject::connect(rrcUi.cm_edit, SIGNAL(returnPressed()),
                   this, SLOT(modify()));
  // RRC protocol tab
  QObject::connect(rrcUi.rrc_amplitude_edit, SIGNAL(returnPressed()),
                   this, SLOT(modify()));
  QObject::connect(rrcUi.rrc_delay_edit, SIGNAL(returnPressed()),
                   this, SLOT(modify()));
  QObject::connect(rrcUi.rrc_length_edit, SIGNAL(returnPressed()),
                   this, SLOT(modify()));
  QObject::connect(rrcUi.rrc_thresholdWindow_edit, SIGNAL(returnPressed()),
                   this, SLOT(modify()));
  QObject::connect(rrcUi.rrc_beatNumber_edit, SIGNAL(returnPressed()),
                   this, SLOT(modify()));
  QObject::connect(rrcUi.rrc_chance_edit, SIGNAL(returnPressed()),
                   this, SLOT(modify()));
  QObject::connect(rrcUi.rrc_endBeatNumber_edit, SIGNAL(returnPressed()),
                   this, SLOT(modify()));
  // APD tab
  QObject::connect(rrcUi.apd_repolPercent_edit, SIGNAL(returnPressed()),
                   this, SLOT(modify()));
  QObject::connect(rrcUi.apd_min_edit, SIGNAL(returnPressed()),
                   this, SLOT(modify()));
  QObject::connect(rrcUi.apd_stimWindow_edit, SIGNAL(returnPressed()),
                   this, SLOT(modify()));
  // Data tab
  QObject::connect(rrcUi.stimThreshold_dataCheck, SIGNAL(clicked()),
                   this, SLOT(modify()));
  QObject::connect(rrcUi.pace_dataCheck, SIGNAL(clicked()),
                   this, SLOT(modify()));
  QObject::connect(rrcUi.rrcThreshold_dataCheck, SIGNAL(clicked()),
                   this, SLOT(modify()));
  QObject::connect(rrcUi.rrcProtocol_dataCheck, SIGNAL(clicked()),
                   this, SLOT(modify()));
  // Timer
  QObject::connect(timer, SIGNAL(timeout()),
                   this, SLOT(refreshDisplay()));

  // Connections to allow only one button being toggled at a time
  // Stim threshold button
  QObject::connect(rrcUi.stimThreshold_button, SIGNAL(toggled(bool)),
                   rrcUi.pace_button, SLOT(setDisabled(bool)));
  QObject::connect(rrcUi.stimThreshold_button, SIGNAL(toggled(bool)),
                   rrcUi.rrcProtocol_button, SLOT(setDisabled(bool)));
  QObject::connect(rrcUi.stimThreshold_button, SIGNAL(toggled(bool)),
                   rrcUi.rrcThreshold_button, SLOT(setDisabled(bool)));
  // Pace button
  QObject::connect(rrcUi.pace_button, SIGNAL(toggled(bool)),
                   rrcUi.stimThreshold_button, SLOT(setDisabled(bool)));
  QObject::connect(rrcUi.pace_button, SIGNAL(toggled(bool)),
                   rrcUi.rrcThreshold_button, SLOT(setDisabled(bool)));
  QObject::connect(rrcUi.pace_button, SIGNAL(toggled(bool)),
                   rrcUi.rrcProtocol_button, SLOT(setDisabled(bool)));
  // RRC threshold button
  QObject::connect(rrcUi.rrcThreshold_button, SIGNAL(toggled(bool)),
                   rrcUi.stimThreshold_button, SLOT(setDisabled(bool)));
  QObject::connect(rrcUi.rrcThreshold_button, SIGNAL(toggled(bool)),
                   rrcUi.pace_button, SLOT(setDisabled(bool)));
  QObject::connect(rrcUi.rrcThreshold_button, SIGNAL(toggled(bool)),
                   rrcUi.rrcProtocol_button, SLOT(setDisabled(bool)));
  // RRC protocol button
  QObject::connect(rrcUi.rrcProtocol_button, SIGNAL(toggled(bool)),
                   rrcUi.stimThreshold_button, SLOT(setDisabled(bool)));
  QObject::connect(rrcUi.rrcProtocol_button, SIGNAL(toggled(bool)),
                   rrcUi.pace_button, SLOT(setDisabled(bool)));
  QObject::connect(rrcUi.rrcProtocol_button, SIGNAL(toggled(bool)),
                   rrcUi.rrcThreshold_button, SLOT(setDisabled(bool)));

  subWindow->show();
  subWindow->adjustSize();
}

void RRC::Module::initialize() {
  // Workspace states
  time = 0;
  voltage = 0;
  beatNumber = 0;
  apd = 0;
  // Connect states to workspace
  Workspace::Instance::setData(Workspace::STATE, 0, &time);
  Workspace::Instance::setData(Workspace::STATE, 1, &voltage);
  Workspace::Instance::setData(Workspace::STATE, 2, &beatNumber);
  Workspace::Instance::setData(Workspace::STATE, 3, &apd);

  // Workspace parameters
  //// Stimulus tab
  bcl = 1000;
  stim_amplitude = 4;
  stim_length = 1;
  ljp = 0;
  cm = 100;
  //// RRC threshold tab
  thresh_startAmplitude = 0;
  thresh_ampIncrement = 0.01;
  thresh_beatNumber = 3;
  thresh_apdCutoff = 20;
  //// RRC protocol tab
  rrc_amplitude = 0;
  rrc_delay = 5;
  rrc_length = 0;
  rrc_thresholdWindow = 10;
  rrc_beatNumber = 3;
  rrc_chance = 50;
  rrc_endBeatNumber = 100;
  //// APD tab
  apd_repolPercent = 90;
  apd_min = 50;
  apd_stimWindow = 4;
  //// Data tab
  pace_recordData = false;
  stim_recordData = false;
  thresh_recordData = false;
  rrcProtocol_recordData = false;

  // Set user interface values
  //// Stimulus tab
  rrcUi.bcl_edit->setText(QString::number(bcl));
  rrcUi.stim_amplitude_edit->setText(QString::number(stim_amplitude));
  rrcUi.stim_length_edit->setText(QString::number(stim_length));
  rrcUi.ljp_edit->setText(QString::number(ljp));
  rrcUi.cm_edit->setText(QString::number(cm));
  //// RRC threshold tab
  rrcUi.thresh_startAmplitude_edit->
      setText(QString::number(thresh_startAmplitude));
  rrcUi.thresh_ampIncrement_edit->setText(QString::number(thresh_ampIncrement));
  rrcUi.thresh_beatNumber_edit->setText(QString::number(thresh_beatNumber));
  rrcUi.thresh_apdCutoff_edit->setText(QString::number(thresh_apdCutoff));
  //// RRC protocol tab
  rrcUi.rrc_amplitude_edit->setText(QString::number(rrc_amplitude));
  rrcUi.rrc_delay_edit->setText(QString::number(rrc_delay));
  rrcUi.rrc_length_edit->setText(QString::number(rrc_length));
  rrcUi.rrc_thresholdWindow_edit->setText(QString::number(rrc_thresholdWindow));
  rrcUi.rrc_beatNumber_edit->setText(QString::number(rrc_beatNumber));
  rrcUi.rrc_chance_edit->setText(QString::number(rrc_chance));
  rrcUi.rrc_endBeatNumber_edit->setText(QString::number(rrc_endBeatNumber));
  //// APD tab
  rrcUi.apd_repolPercent_edit->setText(QString::number(apd_repolPercent));
  rrcUi.apd_min_edit->setText(QString::number(apd_min));
  rrcUi.apd_stimWindow_edit->setText(QString::number(apd_stimWindow));
  //// Data tab
  rrcUi.stimThreshold_dataCheck->setChecked(stim_recordData);
  rrcUi.pace_dataCheck->setChecked(pace_recordData);
  rrcUi.rrcThreshold_dataCheck->setChecked(thresh_recordData);
  rrcUi.rrcProtocol_dataCheck->setChecked(rrcProtocol_recordData);
}

// Slot Functions
void RRC::Module::refreshDisplay() {
  rrcUi.time_display->display(time);
  rrcUi.voltage_display->display(voltage);
  rrcUi.beatNumber_display->display(beatNumber);
  rrcUi.apd_display->display(apd);

  if (execute_mode == IDLE) {
    if (rrcUi.stimThreshold_button->isChecked() && !stim_onFlag) {
      rrcUi.stimThreshold_button->setChecked(false);
      rrcUi.stim_amplitude_edit->setText(QString::number(stim_amplitude));
      modify();
    }
    if (rrcUi.rrcThreshold_button->isChecked() && !thresh_onFlag) {
      rrcUi.rrcThreshold_button->setChecked(false);
      rrcUi.rrc_amplitude_edit->setText(QString::number(thresh_rrcAmplitude));
      rrcUi.rrc_thresholdTest_display->display(thresh_rrcAmplitude);
      modify();
    }
    else if (rrcUi.rrcProtocol_button->isChecked() && !rrcProtocol_onFlag) {
      rrcUi.rrcProtocol_button->setChecked(false);
    }
  }
  else if (execute_mode == RRCPROTOCOL) {
    if (beatNumber_int % rrc_beatNumber == 0 &&
        rrc_random_injection <= rrc_chance) {
      if (rrc_random_threshold >= 50)
        rrcUi.rrc_chance_display->display(1);
      else
        rrcUi.rrc_chance_display->display(-1);
    }
    else
      rrcUi.rrc_chance_display->display(0);
  }
}

void RRC::Module::modify() {
  bool active = getActive();
  // Make sure real-time thread is not in the middle of execution
  setActive(false);
  RRC_SyncEvent event;
  RT::System::getInstance()->postEvent(&event);

  // Get user interface values
  //// Stimulus tab
  bcl = rrcUi.bcl_edit->text().toDouble();
  stim_amplitude = rrcUi.stim_amplitude_edit->text().toDouble();
  stim_length = rrcUi.stim_length_edit->text().toDouble();
  ljp = rrcUi.ljp_edit->text().toDouble();
  cm = rrcUi.cm_edit->text().toDouble();
  //// RRC threshold tab
  thresh_startAmplitude = rrcUi.thresh_startAmplitude_edit->text().toDouble();
  thresh_ampIncrement = rrcUi.thresh_ampIncrement_edit->text().toDouble();
  thresh_beatNumber = rrcUi.thresh_beatNumber_edit->text().toInt();
  thresh_apdCutoff = rrcUi.thresh_apdCutoff_edit->text().toInt();
  //// RRC protocol tab
  rrc_amplitude = rrcUi.rrc_amplitude_edit->text().toDouble();
  rrc_delay = rrcUi.rrc_delay_edit->text().toDouble();
  rrc_length = rrcUi.rrc_length_edit->text().toInt();
  rrc_thresholdWindow = rrcUi.rrc_thresholdWindow_edit->text().toInt();
  rrc_beatNumber = rrcUi.rrc_beatNumber_edit->text().toInt();
  rrc_chance = rrcUi.rrc_chance_edit->text().toInt();
  rrc_endBeatNumber = rrcUi.rrc_endBeatNumber_edit->text().toInt();
  //// APD tab
  apd_repolPercent = rrcUi.apd_repolPercent_edit->text().toInt();
  apd_min = rrcUi.apd_min_edit->text().toInt();
  apd_stimWindow = rrcUi.apd_stimWindow_edit->text().toInt();
  //// Data tab
  stim_recordData = rrcUi.stimThreshold_dataCheck->isChecked();
  pace_recordData = rrcUi.pace_dataCheck->isChecked();
  thresh_recordData = rrcUi.rrcThreshold_dataCheck->isChecked();
  rrcProtocol_recordData = rrcUi.rrcProtocol_dataCheck->isChecked();

  // Set parameters to workspace
  setValue(0, bcl);
  setValue(1, stim_amplitude);
  setValue(2, stim_length);
  setValue(3, ljp);
  setValue(4, cm);
  setValue(5, thresh_startAmplitude);
  setValue(6, thresh_ampIncrement);
  setValue(7, thresh_beatNumber);
  setValue(8, thresh_apdCutoff);
  setValue(9, rrc_amplitude);
  setValue(10, rrc_delay);
  setValue(11, rrc_length);
  setValue(12, rrc_thresholdWindow);
  setValue(13, rrc_beatNumber);
  setValue(14, rrc_chance);
  setValue(15, rrc_endBeatNumber);
  setValue(16, apd_repolPercent);
  setValue(17, apd_min);
  setValue(18, apd_stimWindow);

  setActive(active);
}

// Data recording functions
void RRC::Module::dataRecord_start() {
  Event::Object event(Event::START_RECORDING_EVENT);
  Event::Manager::getInstance()->postEventRT(&event);
  recording = true;
}

void RRC::Module::dataRecord_stop() {
  Event::Object event(Event::STOP_RECORDING_EVENT);
  Event::Manager::getInstance()->postEventRT(&event);
  recording = false;
}

void RRC::Module::reset() {
  // Grabs RTXI thread period and converts to ms (from ns)
  period = RT::System::getInstance()->getPeriod() * 1e-6;

  bcl_int = bcl / period;
  stim_length_int = stim_length / period;

  time = -period;
  time_int = -1;
  bcl_startTime = 0;
  beatNumber = 1;
  beatNumber_int = 1;

  calculateAPD(1);
}

// APD calculation function
void RRC::Module::calculateAPD(int step) {
  switch (step) {
    case 1:
      apd_mode = START;
      break;

    case 2:
      switch(apd_mode) {
        // Find time membrane voltage passes upstroke threshold, start of AP
        case START:
          if (voltage >= apd_upstrokeThreshold) {
            apd_startTime = time;
            apd_peakVoltage = apd_vmRest;
            apd_mode = PEAK;
          }
          // If stimulus fails to produce an AP, set APD to 0
          else if ((time_int - time_int) > 2 * apd_stimWindow / period) {
            apd_mode = DONE;
            apd = 0;
          }
          break;

          // Find peak of AP, points within "window" are ignored to eliminate
          // effect of stimulus artifact
        case PEAK:
          // If we are outside the chosen time window after the AP
          if ((time - apd_startTime) > apd_stimWindow) {
            if (apd_peakVoltage < voltage) { // Find peak voltage
              apd_peakVoltage = voltage;
              apd_peakTime = time;
            }
            // Keep looking for the peak for 5ms to account for noise
            else if ((time - apd_peakTime) > 5) {
              double apd_amplitude;

              // Amplitude of action potential based on resting membrane
              // and peak voltage
              apd_amplitude = apd_peakVoltage - apd_vmRest ;

              // Calculate downstroke threshold based on AP amplitude and
              // desired AP repolarization %
              apd_downstrokeThreshold =
                  apd_peakVoltage -
                  (apd_amplitude * (apd_repolPercent / 100.0));
              apd_mode = DOWN;
            }
          }
          break;

        case DOWN: // Find downstroke threshold and calculate APD
          if (voltage <= apd_downstrokeThreshold) {
            apd_endTime = time;
            apd = time - apd_startTime;
            apd_mode = DONE;
          }
          break;

        default: // DONE: APD has been found, do nothing
          break;
      }
  }
}

// Toggle funcitons
void RRC::Module::toggle_stimThreshold() {
  stim_onFlag = rrcUi.stimThreshold_button->isChecked();

  // Make sure real-time thread is not in the middle of execution
  setActive(false);
  RRC_SyncEvent event;
  RT::System::getInstance()->postEvent(&event);

  // Stimulus threshold
  if (stim_onFlag) {
    execute_mode = STIMTHRESHOLD;
    reset();
    stim_vmRest = input(0) * 1e3 - ljp;
    stim_peakVoltage = stim_vmRest;
    stim_stimulusLevel = 2.0;
    stim_responseDuration = 0;
    stim_responseTime = 0;
    setActive(true);
  }
  else { // If in middle of protocol
    if (recording) {
      ::Event::Object event(::Event::STOP_RECORDING_EVENT);
      ::Event::Manager::getInstance()->postEventRT(&event);
      recording = false;
    }

    execute_mode = IDLE;
    setActive(false);
  }
}

void RRC::Module::toggle_pace() {
  pace_onFlag = rrcUi.pace_button->isChecked();

  // Make sure real-time thread is not in the middle of execution
  setActive(false);
  RRC_SyncEvent event;
  RT::System::getInstance()->postEvent(&event);

  // Start protocol, reinitialize parameters to start values
  if (pace_onFlag) {
    reset();
    execute_mode = PACE;
    setActive(true);
  }
  else { // Called in the middle of protocol
    if (recording) {
      ::Event::Object event(::Event::STOP_RECORDING_EVENT);
      ::Event::Manager::getInstance()->postEventRT(&event);
      recording = false;
    }
    execute_mode = IDLE;
    setActive(false);
  }
}

void RRC::Module::toggle_rrcThreshold() {
  thresh_onFlag = rrcUi.rrcThreshold_button->isChecked();

  // Make sure real-time thread is not in the middle of execution
  setActive(false);
  RRC_SyncEvent event;
  RT::System::getInstance()->postEvent(&event);

  // Start protocol, reinitialize parameters to start values
  if (thresh_onFlag) {
    reset();
    execute_mode = RRCTHRESHOLD;
    thresh_previousAPD = -1;
    thresh_rrcThreshFound = false;
    thresh_rrcAmplitude = thresh_startAmplitude;
    setActive(true);
  }
  else { // Called when in the middle of protocol
    if (recording) {
      ::Event::Object event(::Event::STOP_RECORDING_EVENT);
      ::Event::Manager::getInstance()->postEventRT(&event);
      recording = false;
    }
    execute_mode = IDLE;
    setActive(false);
  }

}

void RRC::Module::toggle_rrcProtocol() {
  rrcProtocol_onFlag = rrcUi.rrcProtocol_button->isChecked();

  // Make sure real-time thread is not in the middle of execution
  setActive(false);
  RRC_SyncEvent event;
  RT::System::getInstance()->postEvent(&event);

  // Start protocol, reinitialize parameters to start values
  if (rrcProtocol_onFlag) {
    reset();
    execute_mode = RRCPROTOCOL;
    setActive(true);
  }
  else { // Called when in the middle of protocol
    if (recording) {
      ::Event::Object event(::Event::STOP_RECORDING_EVENT);
      ::Event::Manager::getInstance()->postEventRT(&event);
      recording = false;
    }
    execute_mode = IDLE;
    setActive(false);
  }
}

// Event handling
void RRC::Module::receiveEvent( const ::Event::Object *event ) {
}

void RRC::Module::receiveEventRT( const ::Event::Object *event ) {
}

// Settings loading and saving
void RRC::Module::doLoad(const Settings::Object::State &s) {
  if (s.loadInteger("Maximized")) showMaximized();
  else if (s.loadInteger("Minimized")) showMinimized();

  if (s.loadInteger("W")) {
    subWindow->resize(s.loadInteger("W"), s.loadInteger("H"));
    parentWidget()->move(s.loadInteger("X"), s.loadInteger("Y"));
  }

  // Workspace parameters
  //// Stimulus tab
  bcl = s.loadDouble("bcl");
  stim_amplitude = s.loadDouble("stim_amplitude");
  stim_length = s.loadDouble("stim_length");
  ljp = s.loadDouble("ljp");
  cm = s.loadDouble("cm");
  //// RRC threshold tab
  thresh_startAmplitude = s.loadDouble("thresh_startAmplitude");
  thresh_ampIncrement = s.loadDouble("thresh_ampIncrement");
  thresh_beatNumber = s.loadInteger("thresh_beatNumber");
  thresh_apdCutoff = s.loadInteger("thresh_apdCutoff");
  //// RRC protocol tab
  rrc_amplitude = s.loadDouble("rrc_amplitude");
  rrc_delay = s.loadDouble("rrc_delay");
  rrc_length = s.loadInteger("rrc_length");
  rrc_thresholdWindow = s.loadInteger("rrc_thresholdWindow");
  rrc_beatNumber = s.loadInteger("rrc_beatNumber");
  rrc_chance = s.loadInteger("rrc_chance");
  rrc_endBeatNumber = s.loadInteger("rrc_endBeatNumber");
  //// APD tab
  apd_repolPercent = s.loadInteger("apd_repolPercent");
  apd_min = s.loadInteger("apd_min");
  apd_stimWindow = s.loadInteger("apd_stimWindow");
  //// Data tab
  pace_recordData = s.loadInteger("pace_recordData");
  stim_recordData = s.loadInteger("stim_recordData");
  thresh_recordData = s.loadInteger("thresh_recordData");
  rrcProtocol_recordData = s.loadInteger("rrcProtocol_recordData");

  // Set user interface values
  //// Stimulus tab
  rrcUi.bcl_edit->setText(QString::number(bcl));
  rrcUi.stim_amplitude_edit->setText(QString::number(stim_amplitude));
  rrcUi.stim_length_edit->setText(QString::number(stim_length));
  rrcUi.ljp_edit->setText(QString::number(ljp));
  rrcUi.cm_edit->setText(QString::number(cm));
  //// RRC threshold tab
  rrcUi.thresh_startAmplitude_edit->
      setText(QString::number(thresh_startAmplitude));
  rrcUi.thresh_ampIncrement_edit->setText(QString::number(thresh_ampIncrement));
  rrcUi.thresh_beatNumber_edit->setText(QString::number(thresh_beatNumber));
  rrcUi.thresh_apdCutoff_edit->setText(QString::number(thresh_apdCutoff));
  //// RRC protocol tab
  rrcUi.rrc_amplitude_edit->setText(QString::number(rrc_amplitude));
  rrcUi.rrc_delay_edit->setText(QString::number(rrc_delay));
  rrcUi.rrc_length_edit->setText(QString::number(rrc_length));
  rrcUi.rrc_thresholdWindow_edit->setText(QString::number(rrc_thresholdWindow));
  rrcUi.rrc_beatNumber_edit->setText(QString::number(rrc_beatNumber));
  rrcUi.rrc_chance_edit->setText(QString::number(rrc_chance));
  rrcUi.rrc_endBeatNumber_edit->setText(QString::number(rrc_endBeatNumber));
  //// APD tab
  rrcUi.apd_repolPercent_edit->setText(QString::number(apd_repolPercent));
  rrcUi.apd_min_edit->setText(QString::number(apd_min));
  rrcUi.apd_stimWindow_edit->setText(QString::number(apd_stimWindow));
  //// Data tab
  rrcUi.stimThreshold_dataCheck->setChecked(stim_recordData);
  rrcUi.pace_dataCheck->setChecked(pace_recordData);
  rrcUi.rrcThreshold_dataCheck->setChecked(thresh_recordData);
  rrcUi.rrcProtocol_dataCheck->setChecked(rrcProtocol_recordData);
}

void RRC::Module::doSave(Settings::Object::State &s) const {
  // Window settings
  if (subWindow->isMaximized())
    s.saveInteger("Maximized", 1);
  else if (subWindow->isMinimized())
    s.saveInteger("Minimized", 1);

  QPoint pos = subWindow->pos();
  s.saveInteger("X", pos.x());
  s.saveInteger("Y", pos.y());
  s.saveInteger("W", subWindow->width());
  s.saveInteger("H", subWindow->height());

  // Parameters
  //// Stimulus Tab
  s.saveDouble("bcl", bcl);
  s.saveDouble("stim_amplitude", stim_amplitude);
  s.saveDouble("stim_length", stim_length);
  s.saveDouble("ljp", ljp);
  s.saveDouble("cm", cm);
  //// RRC threshold tab
  s.saveDouble("thresh_startAmplitude", thresh_startAmplitude);
  s.saveDouble("thresh_ampIncrement", thresh_ampIncrement);
  s.saveInteger("thresh_beatNumber", thresh_beatNumber);
  s.saveInteger("thresh_apdCutoff", thresh_apdCutoff);
  //// RRC protocol tab
  s.saveDouble("rrc_amplitude", rrc_amplitude);
  s.saveDouble("rrc_delay", rrc_delay);
  s.saveInteger("rrc_length", rrc_length);
  s.saveInteger("rrc_thresholdWindow", rrc_thresholdWindow);
  s.saveInteger("rrc_beatNumber", rrc_beatNumber);
  s.saveInteger("rrc_chance", rrc_chance);
  s.saveInteger("rrc_endBeatNumber", rrc_endBeatNumber);
  //// APD tab
  s.saveInteger("apd_repolPercent", apd_repolPercent);
  s.saveInteger("apd_min", apd_min);
  s.saveInteger("apd_stimWindow", apd_stimWindow);
  //// Data tab
  s.saveInteger("stim_recordData", rrcUi.stimThreshold_dataCheck->isChecked());
  s.saveInteger("pace_recordData", rrcUi.pace_dataCheck->isChecked());
  s.saveInteger("thresh_recordData", rrcUi.rrcThreshold_dataCheck->isChecked());
  s.saveInteger("rrcProtocol_recordData",
                rrcUi.rrcProtocol_dataCheck->isChecked());
}
