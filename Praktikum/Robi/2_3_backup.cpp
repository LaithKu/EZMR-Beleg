#include <Arduino.h>
#include <micro_ros_platformio.h>

#include <rcl/rcl.h>
#include <rclc/rclc.h>
#include <rclc/executor.h>

#include <geometry_msgs/msg/twist.h>

#if !defined(MICRO_ROS_TRANSPORT_ARDUINO_WIFI)
#error This example is only available for Arduino framework with wifi transport.
#endif

char ssid[] = "WLAN-Name";
char psk[]  = "Passwort";

IPAddress agent_ip(192, 168, 2, 218);
size_t agent_port = 8888;

#define MOTOR_TX_PIN 17
#define START_BYTE   0xAA

#define LEFT_MOTOR_ID  1
#define RIGHT_MOTOR_ID 2

#define MODE_CONTINUOUS 1
#define MOTOR_PARAM     3

HardwareSerial MotorSerial(2);


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

void driveForward()
{
// Links vorwaerts
    sendMotorCommand(
        LEFT_MOTOR_ID,
        MODE_CONTINUOUS,
        1,
        MOTOR_PARAM
    );

// Rechts vorwaerts
    sendMotorCommand(
        RIGHT_MOTOR_ID,
        MODE_CONTINUOUS,
        0,
        MOTOR_PARAM
    );
}

void driveBackward()
{
    // Links rueckwaerts
    sendMotorCommand(
        LEFT_MOTOR_ID,
        MODE_CONTINUOUS,
        0,
        MOTOR_PARAM
    );

    // Rechts rueckwaerts
    sendMotorCommand(
        RIGHT_MOTOR_ID,
        MODE_CONTINUOUS,
        1,
        MOTOR_PARAM
    );
}

void turnLeft()
{
    // Linkes Rad rueckwaerts
    sendMotorCommand(
        LEFT_MOTOR_ID,
        MODE_CONTINUOUS,
        0,
        MOTOR_PARAM
    );

    // Rechtes Rad vorwaerts
    sendMotorCommand(
        RIGHT_MOTOR_ID,
        MODE_CONTINUOUS,
        0,
        MOTOR_PARAM
    );
}

void turnRight()
{
    // Linkes Rad vorwaerts
    sendMotorCommand(
        LEFT_MOTOR_ID,
        MODE_CONTINUOUS,
        1,
        MOTOR_PARAM
    );

    // Rechtes Rad rueckwaerts
    sendMotorCommand(
        RIGHT_MOTOR_ID,
        MODE_CONTINUOUS,
        1,
        MOTOR_PARAM
    );
}

rcl_subscription_t subscriber;

geometry_msgs__msg__Twist msg;

rclc_executor_t executor;
rclc_support_t support;
rcl_allocator_t allocator;
rcl_node_t node;

#define RCCHECK(fn) { \
    rcl_ret_t temp_rc = fn; \
    if ((temp_rc != RCL_RET_OK)) { \
        error_loop(); \
    } \
}

#define RCSOFTCHECK(fn) { \
    rcl_ret_t temp_rc = fn; \
    if ((temp_rc != RCL_RET_OK)) {} \
}

void error_loop()
{
    while (1)
    {
        delay(100);
    }
}

void subscription_callback(const void *msgin)
{
    const geometry_msgs__msg__Twist *cmd =
        (const geometry_msgs__msg__Twist *)msgin;

    float linear  = cmd->linear.x;
    float angular = cmd->angular.z;

    Serial.print("linear.x = ");
    Serial.print(linear);

    Serial.print(" | angular.z = ");
    Serial.println(angular);

    // Vorwaerts
    if (linear > 0.05 && abs(angular) < 0.05)
    {
        Serial.println("VORWAERTS");
        driveForward();
    }

    // Rueckwaerts
    else if (linear < -0.05 && abs(angular) < 0.05)
    {
        Serial.println("RUECKWAERTS");
        driveBackward();
    }

    // Links
    else if (angular > 0.05)
    {
        Serial.println("LINKS");
        turnLeft();
    }

    // Rechts
    else if (angular < -0.05)
    {
        Serial.println("RECHTS");
        turnRight();
    }

    else
    {
        Serial.println("STOP");
        
        // Stop-Befehl testen
        sendMotorCommand(
            LEFT_MOTOR_ID,
            MODE_CONTINUOUS,
            1,
            0
        );

        sendMotorCommand(
            RIGHT_MOTOR_ID,
            MODE_CONTINUOUS,
            0,
            0
        );
    }
}

void setup()
{
    Serial.begin(115200);

    MotorSerial.begin(
        9600,
        SERIAL_8N1,
        -1,
        MOTOR_TX_PIN
    );

    set_microros_wifi_transports(
        ssid,
        psk,
        agent_ip,
        agent_port
    );

    delay(2000);


    allocator = rcl_get_default_allocator();

    // ROS_DOMAIN_ID = 1
    rcl_init_options_t init_options =
        rcl_get_zero_initialized_init_options();

    RCCHECK(
        rcl_init_options_init(
            &init_options,
            allocator
        )
    );

    RCCHECK(
        rcl_init_options_set_domain_id(
            &init_options,
            1
        )
    );

    RCCHECK(
        rclc_support_init_with_options(
            &support,
            0,
            NULL,
            &init_options,
            &allocator
        )
    );

    RCCHECK(
        rclc_node_init_default(
            &node,
            "micro_ros_robot",
            "",
            &support
        )
    );

    RCCHECK(
        rclc_subscription_init_default(
            &subscriber,
            &node,
            ROSIDL_GET_MSG_TYPE_SUPPORT(
                geometry_msgs,
                msg,
                Twist
            ),
            "/cmd_vel"
        )
    );

    RCCHECK(
        rclc_executor_init(
            &executor,
            &support.context,
            1,
            &allocator
        )
    );

    RCCHECK(
        rclc_executor_add_subscription(
            &executor,
            &subscriber,
            &msg,
            &subscription_callback,
            ON_NEW_DATA
        )
    );

    Serial.println("Roboter bereit");
}

void loop()
{
    RCSOFTCHECK(
        rclc_executor_spin_some(
            &executor,
            RCL_MS_TO_NS(20)
        )
    );

    delay(10);
}
