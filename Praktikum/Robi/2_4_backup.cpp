#include <Arduino.h>
#include <Wire.h>

#include <micro_ros_platformio.h>

#include <rcl/rcl.h>
#include <rcl/error_handling.h>
#include <rclc/rclc.h>
#include <rclc/executor.h>

#include <geometry_msgs/msg/twist.h>
#include <std_msgs/msg/float32.h>

#define WIFI_SSID       "WLAN-Name"
#define WIFI_PASSWORD   "Passwort"

IPAddress agent_ip(192, 168, 2, 218);
uint16_t agent_port = 8888;

#define MOTOR_TX_PIN 17
#define START_BYTE   0xAA

#define LEFT_MOTOR_ID   1
#define RIGHT_MOTOR_ID  2
#define MOTOR_PARAM 3

HardwareSerial MotorSerial(2);


#define TRIG_PIN 26
#define ECHO_PIN 25

#define SDA_PIN 21
#define SCL_PIN 22

#define MPU_ADDR       0x68
#define PWR_MGMT_1     0x6B
#define GYRO_CONFIG    0x1B
#define GYRO_ZOUT_H    0x47

float gyroBiasZ = 0.0;

rcl_node_t node;

rcl_subscription_t cmd_vel_subscriber;

rcl_publisher_t distance_publisher;
rcl_publisher_t gyro_publisher;

geometry_msgs__msg__Twist cmd_vel_msg;

std_msgs__msg__Float32 distance_msg;
std_msgs__msg__Float32 gyro_msg;

rclc_executor_t executor;

rcl_allocator_t allocator;
rclc_support_t support;

#define RCCHECK(fn) {                                      \
    rcl_ret_t temp_rc = fn;                                \
    if ((temp_rc != RCL_RET_OK))                           \
    {                                                      \
        Serial.print("micro-ROS Fehler: ");                \
        Serial.println(temp_rc);                           \
        errorLoop();                                       \
    }                                                      \
}

#define RCSOFTCHECK(fn) {                                  \
    rcl_ret_t temp_rc = fn;                                \
    if ((temp_rc != RCL_RET_OK))                           \
    {                                                      \
        Serial.print("micro-ROS Publish Fehler: ");        \
        Serial.println(temp_rc);                           \
    }                                                      \
}

void errorLoop()
{
    while (1)
    {
        delay(100);
    }
}

void sendMotorCommand(
    uint8_t id,
    uint8_t mode,
    uint8_t dir,
    uint16_t param)
{
    MotorSerial.write(START_BYTE);
    MotorSerial.write(id);
    MotorSerial.write(mode);
    MotorSerial.write(dir);

    MotorSerial.write((uint8_t)((param >> 8) & 0xFF));
    MotorSerial.write((uint8_t)(param & 0xFF));

    MotorSerial.flush();
}

void stopMotors()
{
    sendMotorCommand(LEFT_MOTOR_ID, 1, 1, 0);
    sendMotorCommand(RIGHT_MOTOR_ID, 1, 0, 0);
}

void moveForward()
{

    sendMotorCommand(
        LEFT_MOTOR_ID,
        1,
        1,
        MOTOR_PARAM);

    sendMotorCommand(
        RIGHT_MOTOR_ID,
        1,
        0,
        MOTOR_PARAM);
}

void moveBackward()
{

    sendMotorCommand(
        LEFT_MOTOR_ID,
        1,
        0,
        MOTOR_PARAM);

    sendMotorCommand(
        RIGHT_MOTOR_ID,
        1,
        1,
        MOTOR_PARAM);
}

void turnLeft()
{

    sendMotorCommand(
        LEFT_MOTOR_ID,
        1,
        0,
        MOTOR_PARAM);

    sendMotorCommand(
        RIGHT_MOTOR_ID,
        1,
        0,
        MOTOR_PARAM);
}

void turnRight()
{

    sendMotorCommand(
        LEFT_MOTOR_ID,
        1,
        1,
        MOTOR_PARAM);

    sendMotorCommand(
        RIGHT_MOTOR_ID,
        1,
        1,
        MOTOR_PARAM);
}

void cmdVelCallback(const void *msgin)
{
    const geometry_msgs__msg__Twist *msg =
        (const geometry_msgs__msg__Twist *)msgin;

    float linear = msg->linear.x;
    float angular = msg->angular.z;

    Serial.print("cmd_vel: linear=");
    Serial.print(linear);
    Serial.print(" angular=");
    Serial.println(angular);

    // Vorwaerts
    if (linear > 0.05 && fabs(angular) < 0.05)
    {
        moveForward();
    }

    // Rückwaerts
    else if (linear < -0.05 && fabs(angular) < 0.05)
    {
        moveBackward();
    }

    // Links
    else if (angular > 0.05)
    {
        turnLeft();
    }

    // Rechts
    else if (angular < -0.05)
    {
        turnRight();
    }

    // Stop
    else
    {
        stopMotors();
    }
}

float measureDistance()
{
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);

    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);

    digitalWrite(TRIG_PIN, LOW);

    unsigned long duration =
        pulseIn(ECHO_PIN, HIGH, 30000);

    if (duration == 0)
    {
        return -1.0;
    }

    float distance_cm =
        duration * 0.0343 / 2.0;

    return distance_cm / 100.0;
}

void writeMPURegister(uint8_t reg, uint8_t value)
{
    Wire.beginTransmission(MPU_ADDR);
    Wire.write(reg);
    Wire.write(value);
    Wire.endTransmission();
}

int16_t readGyroZRaw()
{
    Wire.beginTransmission(MPU_ADDR);
    Wire.write(GYRO_ZOUT_H);

    Wire.endTransmission(false);

    Wire.requestFrom(
        MPU_ADDR,
        (uint8_t)2,
        (uint8_t)true);

    if (Wire.available() < 2)
    {
        return 0;
    }

    int16_t value =
        ((int16_t)Wire.read() << 8) |
        Wire.read();

    return value;
}

float readGyroZ()
{

    return readGyroZRaw() / 65.5;
}

void calibrateGyro()
{
    const int samples = 200;

    float sum = 0.0;

    Serial.println();
    Serial.println("Gyro-Kalibrierung");
    Serial.println("Roboter NICHT bewegen!");

    delay(1000);

    for (int i = 0; i < samples; i++)
    {
        sum += readGyroZ();
        delay(10);
    }

    gyroBiasZ = sum / samples;

    Serial.print("Gyro-Bias Z = ");
    Serial.print(gyroBiasZ, 3);
    Serial.println(" deg/s");

    Serial.println(
        "Gyro-Kalibrierung abgeschlossen");
}

void setup()
{
    Serial.begin(115200);

    delay(1000);

    Serial.println();
    Serial.println("==============================");
    Serial.println("ROS2 Turtlebot startet");
    Serial.println("==============================");

    MotorSerial.begin(
        9600,
        SERIAL_8N1,
        -1,
        MOTOR_TX_PIN);

    stopMotors();

    Serial.println("Motor UART gestartet");

    pinMode(TRIG_PIN, OUTPUT);
    pinMode(ECHO_PIN, INPUT);

    digitalWrite(TRIG_PIN, LOW);

    Serial.println("HC-SR04 gestartet");

    Wire.begin(SDA_PIN, SCL_PIN);

    writeMPURegister(PWR_MGMT_1, 0x00);

    delay(100);

    writeMPURegister(GYRO_CONFIG, 0x08);

    calibrateGyro();

    Serial.println();
    Serial.println("Starte micro-ROS WiFi...");

    set_microros_wifi_transports(
        WIFI_SSID,
        WIFI_PASSWORD,
        agent_ip,
        agent_port);

    delay(2000);

    allocator = rcl_get_default_allocator();


    // ROS Domain ID = 1
    rcl_init_options_t init_options =
        rcl_get_zero_initialized_init_options();

    RCCHECK(
        rcl_init_options_init(
            &init_options,
            allocator));

    RCCHECK(
        rcl_init_options_set_domain_id(
            &init_options,
            1));

    RCCHECK(
        rclc_support_init_with_options(
            &support,
            0,
            NULL,
            &init_options,
            &allocator));

    RCCHECK(
        rclc_node_init_default(
            &node,
            "esp32_robot",
            "",
            &support));

    RCCHECK(
        rclc_subscription_init_default(
            &cmd_vel_subscriber,
            &node,
            ROSIDL_GET_MSG_TYPE_SUPPORT(
                geometry_msgs,
                msg,
                Twist),
            "/cmd_vel"));

    RCCHECK(
        rclc_publisher_init_default(
            &distance_publisher,
            &node,
            ROSIDL_GET_MSG_TYPE_SUPPORT(
                std_msgs,
                msg,
                Float32),
            "/distance"));

    RCCHECK(
        rclc_publisher_init_default(
            &gyro_publisher,
            &node,
            ROSIDL_GET_MSG_TYPE_SUPPORT(
                std_msgs,
                msg,
                Float32),
            "/gyro_z"));

    RCCHECK(
        rclc_executor_init(
            &executor,
            &support.context,
            1,
            &allocator));


    RCCHECK(
        rclc_executor_add_subscription(
            &executor,
            &cmd_vel_subscriber,
            &cmd_vel_msg,
            &cmdVelCallback,
            ON_NEW_DATA));


    Serial.println();
    Serial.println("==============================");
    Serial.println("micro-ROS bereit");
    Serial.println("Topics:");
    Serial.println("  SUB: /cmd_vel");
    Serial.println("  PUB: /distance");
    Serial.println("  PUB: /gyro_z");
    Serial.println("==============================");
}

void loop()
{
    RCSOFTCHECK(
        rclc_executor_spin_some(
            &executor,
            RCL_MS_TO_NS(10)));

    static unsigned long lastSensorTime = 0;

    unsigned long now = millis();
    
    if (now - lastSensorTime >= 100)
    {
        lastSensorTime = now;

        float distance = measureDistance();

        distance_msg.data = distance;

        RCSOFTCHECK(
            rcl_publish(
                &distance_publisher,
                &distance_msg,
                NULL));

        float gyroZ =
            readGyroZ() - gyroBiasZ;

        if (fabs(gyroZ) < 1.0)
        {
            gyroZ = 0.0;
        }

        gyro_msg.data = gyroZ;

        RCSOFTCHECK(
            rcl_publish(
                &gyro_publisher,
                &gyro_msg,
                NULL));

        Serial.print("Distance: ");

        if (distance < 0)
        {
            Serial.print("kein Echo");
        }
        else
        {
            Serial.print(distance, 3);
            Serial.print(" m");
        }

        Serial.print(" | Gyro Z: ");
        Serial.print(gyroZ, 2);
        Serial.println(" deg/s");
    }
}