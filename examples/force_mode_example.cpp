// this is for emacs file handling -*- mode: c++; indent-tabs-mode: nil -*-

// -- BEGIN LICENSE BLOCK ----------------------------------------------
// Copyright 2022 Universal Robots A/S
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//    * Redistributions of source code must retain the above copyright
//      notice, this list of conditions and the following disclaimer.
//
//    * Redistributions in binary form must reproduce the above copyright
//      notice, this list of conditions and the following disclaimer in the
//      documentation and/or other materials provided with the distribution.
//
//    * Neither the name of the {copyright_holder} nor the names of its
//      contributors may be used to endorse or promote products derived from
//      this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
// -- END LICENSE BLOCK ------------------------------------------------

// In a real-world example it would be better to get those values from command line parameters / a
// better configuration system such as Boost.Program_options

#include <ur_client_library/ur/dashboard_client.h>
#include <ur_client_library/ur/ur_driver.h>
#include <ur_client_library/types.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>
#include "ur_client_library/control/reverse_interface.h"

using namespace urcl;

const std::string DEFAULT_ROBOT_IP = "192.168.56.101";
const std::string SCRIPT_FILE = "resources/external_control.urscript";
const std::string OUTPUT_RECIPE = "examples/resources/rtde_output_recipe.txt";
const std::string INPUT_RECIPE = "examples/resources/rtde_input_recipe.txt";
const std::string CALIBRATION_CHECKSUM = "calib_12788084448423163542";

std::unique_ptr<UrDriver> g_my_driver;
std::unique_ptr<DashboardClient> g_my_dashboard;

std::condition_variable g_program_running_cv;
std::mutex g_program_running_mutex;
bool g_program_running;

// We need a callback function to register. See UrDriver's parameters for details.
void handleRobotProgramState(bool program_running)
{
  // Print the text in green so we see it better
  std::cout << "\033[1;32mProgram running: " << std::boolalpha << program_running << "\033[0m\n" << std::endl;
  if (program_running)
  {
    std::lock_guard<std::mutex> lk(g_program_running_mutex);
    g_program_running = program_running;
    g_program_running_cv.notify_one();
  }
}

void sendFreedriveMessageOrDie(const control::FreedriveControlMessage freedrive_action)
{
  bool ret = g_my_driver->writeFreedriveControlMessage(freedrive_action);
  if (!ret)
  {
    URCL_LOG_ERROR("Could not send joint command. Is the robot in remote control?");
    exit(1);
  }
}

bool waitForProgramRunning(int milliseconds = 100)
{
  std::unique_lock<std::mutex> lk(g_program_running_mutex);
  if (g_program_running_cv.wait_for(lk, std::chrono::milliseconds(milliseconds)) == std::cv_status::no_timeout ||
      g_program_running == true)
  {
    return true;
  }
  return false;
}

int main(int argc, char* argv[])
{
  urcl::setLogLevel(urcl::LogLevel::INFO);
  // Parse the ip arguments if given
  std::string robot_ip = DEFAULT_ROBOT_IP;
  if (argc > 1)
  {
    robot_ip = std::string(argv[1]);
  }

  // Parse how many seconds to run
  auto second_to_run = std::chrono::seconds(0);
  if (argc > 2)
  {
    second_to_run = std::chrono::seconds(std::stoi(argv[2]));
  }

  // Making the robot ready for the program by:
  // Connect the robot Dashboard
  g_my_dashboard.reset(new DashboardClient(robot_ip));
  if (!g_my_dashboard->connect())
  {
    URCL_LOG_ERROR("Could not connect to dashboard");
    return 1;
  }

  // // Stop program, if there is one running
  if (!g_my_dashboard->commandStop())
  {
    URCL_LOG_ERROR("Could not send stop program command");
    return 1;
  }

  // Power it off
  // if (!g_my_dashboard->commandPowerOff())
  //{
  // URCL_LOG_ERROR("Could not send Power off command");
  // return 1;
  //}

  // Power it on
  // if (!g_my_dashboard->commandPowerOn())
  //{
  // URCL_LOG_ERROR("Could not send Power on command");
  // return 1;
  //}

  // Release the brakes
  // if (!g_my_dashboard->commandBrakeRelease())
  //{
  // URCL_LOG_ERROR("Could not send BrakeRelease command");
  // return 1;
  //}

  // Now the robot is ready to receive a program
  std::unique_ptr<ToolCommSetup> tool_comm_setup;
  const bool headless = true;
  g_my_driver.reset(new UrDriver(robot_ip, SCRIPT_FILE, OUTPUT_RECIPE, INPUT_RECIPE, &handleRobotProgramState, headless,
                                 std::move(tool_comm_setup)));

  if (!g_my_driver->checkCalibration(CALIBRATION_CHECKSUM))
  {
    URCL_LOG_ERROR("Calibration checksum does not match actual robot.");
    URCL_LOG_ERROR("Use the ur_calibration tool to extract the correct calibration from the robot and pass that into "
                   "the description. See "
                   "[https://github.com/UniversalRobots/Universal_Robots_ROS_Driver#extract-calibration-information] "
                   "for details.");
  }

  // Make sure that external control script is running
  if (!waitForProgramRunning())
  {
    URCL_LOG_ERROR("External Control script not running.");
    return 1;
  }
  // End of initialization -- We've started the external control program, which means we have to
  // write keepalive signals from now on. Otherwise the connection will be dropped.

  // Start force mode
  // Task frame at the robot's base with limits being large enough to cover the whole workspace
  // Compliance in z axis and rotation around z axis
  bool success;
  if (g_my_driver->getVersion().major < 5)
    success = g_my_driver->startForceMode({ 0, 0, 0, 0, 0, 0 },   // Task frame at the robot's base
                                          { 0, 0, 1, 0, 0, 1 },   // Compliance in z axis and rotation around z axis
                                          { 0, 0, -2, 0, 0, 0 },  // Press in -z direction
                                          2,                      // do not transform the force frame at all
                                          { 0.1, 0.1, 1.5, 3.14, 3.14, 0.5 },  // limits
                                          0.005);  // damping_factor. See ScriptManual for details.
  else
  {
    success = g_my_driver->startForceMode({ 0, 0, 0, 0, 0, 0 },   // Task frame at the robot's base
                                          { 0, 0, 1, 0, 0, 1 },   // Compliance in z axis and rotation around z axis
                                          { 0, 0, -2, 0, 0, 0 },  // Press in -z direction
                                          2,                      // do not transform the force frame at all
                                          { 0.1, 0.1, 1.5, 3.14, 3.14, 0.5 },  // limits
                                          0.005,                               // damping_factor
                                          1.0);  // gain_scaling. See ScriptManual for details.
  }
  if (!success)
  {
    URCL_LOG_ERROR("Failed to start force mode.");
    return 1;
  }

  std::chrono::duration<double> time_done(0);
  std::chrono::duration<double> timeout(second_to_run);
  auto stopwatch_last = std::chrono::steady_clock::now();
  auto stopwatch_now = stopwatch_last;
  while (time_done < timeout || second_to_run.count() == 0)
  {
    g_my_driver->writeKeepalive();

    stopwatch_now = std::chrono::steady_clock::now();
    time_done += stopwatch_now - stopwatch_last;
    stopwatch_last = stopwatch_now;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  URCL_LOG_INFO("Timeout reached.");
  g_my_driver->endForceMode();
}
