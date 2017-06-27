/*
 Copyright (c) 2016, Juan Jimeno
 All rights reserved.
 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions are met:
 * Redistributions of source code must retain the above copyright notice,
 this list of conditions and the following disclaimer.
 * Redistributions in binary form must reproduce the above copyright
 notice, this list of conditions and the following disclaimer in the
 documentation and/or other materials provided with the distribution.
 * Neither the name of  nor the names of its contributors may be used to
 endorse or promote products derived from this software without specific
 prior written permission.
 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 POSSIBILITY OF SUCH DAMAGE.

 NOTES: All Odd Number Motors are on the left side of the robot
        All Even Number Motors are on the right side of the robot
 */
#if (ARDUINO >= 100)
#include <Arduino.h>
#else
#include <WProgram.h>
#endif

#include <ros.h>

//header file for publishing velocities for odom
#include <lino_msgs/Velocities.h>

//header file for cmd_subscribing to "cmd_vel"
#include <geometry_msgs/Twist.h>

//header file for pid server
#include <lino_msgs/PID.h>

//header files for imu
#include <geometry_msgs/Vector3.h>
#include <lino_msgs/Imu.h>

#include <ros/time.h>

#include <Wire.h>

#include "lino_base_config.h"
#include "Encoder.h"
#include "Motor.h"
#include "Kinematics.h"
#include "PID.h"
#include "Imu.h"

#define ENCODER_OPTIMIZE_INTERRUPTS

#define IMU_PUBLISH_RATE 10 //hz
#define VEL_PUBLISH_RATE 10 //hz
#define COMMAND_RATE 15 //hz
#define DEBUG_RATE 5

Encoder motor1_encoder(MOTOR1_ENCODER_A,MOTOR1_ENCODER_B);
Encoder motor2_encoder(MOTOR2_ENCODER_A,MOTOR2_ENCODER_B); 

PID motor1_pid(-255, 255, K_P, K_I, K_D);
PID motor2_pid(-255, 255, K_P, K_I, K_D);

#ifdef L298_DRIVER
    Motor motor1(MOTOR1_PWM, MOTOR1_IN_A, MOTOR1_IN_B); //front-left
    Motor motor2(MOTOR2_PWM, MOTOR2_IN_A, MOTOR2_IN_B); // front-right
#endif

#ifdef BTS7960_DRIVER
    Motor motor1(MOTOR1_IN_A, MOTOR1_IN_B); // front
    Motor motor2(MOTOR2_IN_A, MOTOR2_IN_B); // front
#endif

#if defined BASE_4WD || defined BASE_MECANUM
    Encoder motor3_encoder(MOTOR3_ENCODER_A,MOTOR3_ENCODER_B); 
    Encoder motor4_encoder(MOTOR4_ENCODER_A,MOTOR4_ENCODER_B); 

    PID motor3_pid(-255, 255, K_P, K_I, K_D);
    PID motor4_pid(-255, 255, K_P, K_I, K_D);

    #ifdef L298_DRIVER
        Motor motor3(MOTOR3_PWM, MOTOR3_IN_A, MOTOR3_IN_B); // rear-left
        Motor motor4(MOTOR4_PWM, MOTOR4_IN_A, MOTOR4_IN_B); // rear-right
    #endif

    #ifdef BTS7960_DRIVER
        Motor motor3(MOTOR3_IN_A, MOTOR3_IN_B); // front
        Motor motor4(MOTOR4_IN_A, MOTOR4_IN_B); // frear
    #endif
#endif

#ifdef BASE_ACKERMANN
    #include <Servo.h>
    Servo steering_servo;
#endif

//COUNTS_PER_REV = 0 if no encoder
int Motor::counts_per_rev_ = COUNTS_PER_REV;

Kinematics kinematics(MAX_RPM, WHEEL_DIAMETER, BASE_WIDTH, PWM_BITS);

double g_req_linear_vel_x = 0;
double g_req_linear_vel_y = 0;
double g_req_angular_vel_z = 0;

unsigned long g_prev_command_time = 0;

//callback function prototypes
void commandCallback(const geometry_msgs::Twist& cmd_msg);
void PIDCallback(const lino_msgs::PID& pid);

ros::NodeHandle nh;

ros::Subscriber<geometry_msgs::Twist> cmd_sub("cmd_vel", commandCallback);
ros::Subscriber<lino_msgs::PID> pid_sub("pid", PIDCallback);

lino_msgs::Imu raw_imu_msg;
ros::Publisher raw_imu_pub("raw_imu", &raw_imu_msg);

lino_msgs::Velocities raw_vel_msg;
ros::Publisher raw_vel_pub("raw_vel", &raw_vel_msg);

void setup()
{
    #ifdef BASE_ACKERMANN
        steering_servo.attach(STEERING_PIN);
        steering_servo.write(90); // set servo to mid-point
    #endif 
    
    nh.initNode();
    nh.getHardware()->setBaud(57600);
    nh.subscribe(pid_sub);
    nh.subscribe(cmd_sub);
    nh.advertise(raw_vel_pub);
    nh.advertise(raw_imu_pub);

    while (!nh.connected())
    {
        nh.spinOnce();
    }
    nh.loginfo("LINOBASE CONNECTED");
    delay(1);
}

void loop()
{
    static unsigned long prev_control_time = 0;
    static unsigned long publish_vel_time = 0;
    static unsigned long prev_imu_time = 0;
    static unsigned long prev_debug_time = 0;
    static bool imu_is_initialized;

    //this block drives the robot based on defined rate
    if ((millis() - prev_control_time) >= (1000 / COMMAND_RATE))
    {
        moveBase();
        prev_control_time = millis();
    }

    //this block stops the motor when no command is received
    if ((millis() - g_prev_command_time) >= 400)
    {
        stopBase();
    }

    //this block publishes velocity based on defined rate
    if ((millis() - publish_vel_time) >= (1000 / VEL_PUBLISH_RATE))
    {
        publishVelocities();
        publish_vel_time = millis();
    }

    //this block publishes the IMU data based on defined rate
    if ((millis() - prev_imu_time) >= (1000 / IMU_PUBLISH_RATE))
    {
        //sanity check if the IMU is connected
        if (!imu_is_initialized)
        {
            imu_is_initialized = initIMU();

            if(imu_is_initialized)
                nh.loginfo("IMU Initialized");
            else
                nh.logfatal("IMU failed to initialize. Check your IMU connection.");
        }
        else
        {
            publishIMU();
        }
        prev_imu_time = millis();
    }

    //this block displays the encoder readings. change DEBUG to 0 if you don't want to display
    if(DEBUG)
    {
        if ((millis() - prev_debug_time) >= (1000 / DEBUG_RATE))
        {
            printDebug();
            prev_debug_time = millis();
        }
    }
    //call all the callbacks waiting to be called
    nh.spinOnce();
}

void PIDCallback(const lino_msgs::PID& pid)
{
    //callback function every time PID constants are received from lino_pid for tuning
    //this callback receives pid object where P,I, and D constants are stored
    motor1_pid.updateConstants(pid.p, pid.i, pid.d);
    motor2_pid.updateConstants(pid.p, pid.i, pid.d);

    #if defined BASE_4WD || defined BASE_MECANUM
        motor3_pid.updateConstants(pid.p, pid.i, pid.d);
        motor4_pid.updateConstants(pid.p, pid.i, pid.d);
    #endif
}

void commandCallback(const geometry_msgs::Twist& cmd_msg)
{
    //callback function every time linear and angular speed is received from 'cmd_vel' topic
    //this callback function receives cmd_msg object where linear and angular speed are stored
    g_req_linear_vel_x = cmd_msg.linear.x;
    g_req_linear_vel_y = cmd_msg.linear.y;
    g_req_angular_vel_z = cmd_msg.angular.z;

    g_prev_command_time = millis();
}

void moveBase()
{
    Kinematics::output req_rpm;

    //get the required rpm for each motor based on required velocities
    #if defined BASE_2WD || defined BASE_4WD
        req_rpm = kinematics.getRPM(g_req_linear_vel_x, 0.0 , g_req_angular_vel_z);
    #endif

    #ifdef BASE_ACKERMANN
        steer();
        req_rpm = kinematics.getRPM(g_req_linear_vel_x, 0.0, 0.0);
    #endif

    #ifdef BASE_MECANUM
        req_rpm = kinematics.getRPM(g_req_linear_vel_x, g_req_linear_vel_y, g_req_angular_vel_z);
    #endif

    //the required rpm is capped at -/+ MAX_RPM to prevent the PID from having too much error
    //the PWM value sent to the motor driver is the calculated PID based on required RPM vs measured RPM
    motor1.spin(motor1_pid.compute(constrain(req_rpm.motor1, -MAX_RPM, MAX_RPM), motor1.rpm));
    motor2.spin(motor2_pid.compute(constrain(req_rpm.motor2, -MAX_RPM, MAX_RPM), motor2.rpm));

    #if defined BASE_4WD || defined BASE_MECANUM
        motor3.spin(motor3_pid.compute(constrain(req_rpm.motor3, -MAX_RPM, MAX_RPM), motor3.rpm));  
        motor4.spin(motor4_pid.compute(constrain(req_rpm.motor4, -MAX_RPM, MAX_RPM), motor4.rpm));    
    #endif  
}

void stopBase()
{
    g_req_linear_vel_x = 0.0;
    g_req_linear_vel_y = 0.0;
    g_req_angular_vel_z = 0.0;
}

void steer()
{
    #ifdef BASE_ACKERMANN
        float steering_angle;
        float steering_angle_deg;

        float req_steering_angle = g_req_angular_vel_z;

        //convert steering angle from rad to deg
        steering_angle_deg = req_steering_angle * (180 / PI);

        if(steering_angle_deg > 0)
        {
            //steer left 
            steering_angle = mapFloat(steering_angle_deg, 0, 90, 90, 0);

        }
        else if(steering_angle_deg < 0)
        {
            //steer right
            steering_angle = mapFloat(steering_angle_deg, 0, -90, 90, 180);
        }
        else
        {
            //return steering wheel to middle if there's no command
            steering_angle = 90;
        }
        
        steering_servo.write(steering_angle);
    #endif
}

void publishVelocities()
{
    Kinematics::velocities vel;

    //update the current speed of each motor based on encoder's count
    #if defined BASE_2WD || defined BASE_ACKERMANN
        motor1.updateSpeed(motor1_encoder.read());
        motor2.updateSpeed(motor2_encoder.read());
        vel = kinematics.getVelocities(motor1.rpm, motor2.rpm);
    #endif

    #if defined BASE_4WD || defined BASE_MECANUM
        motor1.updateSpeed(motor1_encoder.read());
        motor2.updateSpeed(motor2_encoder.read());
        motor3.updateSpeed(motor3_encoder.read());
        motor4.updateSpeed(motor4_encoder.read());
        vel = kinematics.getVelocities(motor1.rpm, motor2.rpm, motor3.rpm, motor4.rpm);
    #endif

    //pass velocities to publisher object
    raw_vel_msg.linear_x = vel.linear_x;
    raw_vel_msg.linear_y = vel.linear_y;
    raw_vel_msg.angular_z = vel.angular_z;

    //publish raw_vel_msg object to ROS
    raw_vel_pub.publish(&raw_vel_msg);
}

void publishIMU()
{
    //pass accelerometer data to imu object
    raw_imu_msg.linear_acceleration = readAccelerometer();

    //pass gyroscope data to imu object
    raw_imu_msg.angular_velocity = readGyroscope();

    //pass accelerometer data to imu object
    raw_imu_msg.magnetic_field = readMagnetometer();
    //publish raw_imu_msg object to ROS
    raw_imu_pub.publish(&raw_imu_msg);
}

void printDebug()
{
    char buffer[50];

    sprintf (buffer, "Encoder FrontLeft: %ld", motor1_encoder.read());
    nh.loginfo(buffer);
    sprintf (buffer, "Encoder FrontRight: %ld", motor2_encoder.read());
    nh.loginfo(buffer);

    #if defined BASE_4WD || defined BASE_MECANUM
        sprintf (buffer, "Encoder RearLeft: %ld", motor3_encoder.read());
        nh.loginfo(buffer);
        sprintf (buffer, "Encoder RearRight: %ld", motor4_encoder.read());
        nh.loginfo(buffer);
    #endif
}

float mapFloat(long x, long in_min, long in_max, long out_min, long out_max)
{
    return (float)(x - in_min) * (out_max - out_min) / (float)(in_max - in_min) + out_min;
}