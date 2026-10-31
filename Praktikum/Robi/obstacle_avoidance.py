import rclpy
from rclpy.node import Node

from geometry_msgs.msg import Twist
from std_msgs.msg import Float32


class ObstacleAvoidance(Node):

    def __init__(self):
        super().__init__('obstacle_avoidance')

        self.cmd_pub = self.create_publisher(
            Twist,
            '/cmd_vel',
            10
        )

        self.distance_sub = self.create_subscription(
            Float32,
            '/distance',
            self.distance_callback,
            10
        )

        self.gyro_sub = self.create_subscription(
            Float32,
            '/gyro_z',
            self.gyro_callback,
            10
        )

        self.timer = self.create_timer(
            0.05,
            self.control_loop
        )

        self.DISTANCE_THRESHOLD = 0.25  # Meter
        self.BACKWARD_TIME = 1.0  # Sekunden
        self.STOP_TIME = 0.3  # Sekunden
        self.TARGET_ANGLE = 90.0  # Grad

        self.LINEAR_FORWARD = 1.0
        self.LINEAR_BACKWARD = -1.0
        self.ANGULAR_LEFT = 1.0


        self.state = 'FORWARD'

        self.distance = -1.0
        self.gyro_z = 0.0

        self.angle = 0.0

        self.state_start_time = (
            self.get_clock().now().nanoseconds / 1e9
        )

        self.last_gyro_time = None

        self.get_logger().info(
            'Obstacle Avoidance gestartet'
        )

    def distance_callback(self, msg):

        self.distance = msg.data


    def gyro_callback(self, msg):

        self.gyro_z = msg.data

        if self.state != 'TURN':
            self.last_gyro_time = None
            return

        now = self.get_clock().now().nanoseconds / 1e9

        if self.last_gyro_time is None:
            self.last_gyro_time = now
            return

        dt = now - self.last_gyro_time
        self.last_gyro_time = now

        self.angle += self.gyro_z * dt

    def publish_stop(self):

        msg = Twist()

        msg.linear.x = 0.0
        msg.angular.z = 0.0

        self.cmd_pub.publish(msg)

    def publish_forward(self):

        msg = Twist()

        msg.linear.x = self.LINEAR_FORWARD
        msg.angular.z = 0.0

        self.cmd_pub.publish(msg)

    def publish_backward(self):

        msg = Twist()

        msg.linear.x = self.LINEAR_BACKWARD
        msg.angular.z = 0.0

        self.cmd_pub.publish(msg)

    def publish_turn_left(self):

        msg = Twist()

        msg.linear.x = 0.0
        msg.angular.z = self.ANGULAR_LEFT

        self.cmd_pub.publish(msg)

    def change_state(self, new_state):

        self.state = new_state

        self.state_start_time = (
            self.get_clock().now().nanoseconds / 1e9
        )

        self.get_logger().info(
            f'Neuer Zustand: {new_state}'
        )

    def control_loop(self):

        now = self.get_clock().now().nanoseconds / 1e9

        elapsed = now - self.state_start_time

        if self.state == 'FORWARD':

            self.publish_forward()

            if (
                self.distance > 0.0
                and self.distance < self.DISTANCE_THRESHOLD
            ):

                self.get_logger().info(
                    f'Hindernis erkannt: '
                    f'{self.distance:.3f} m'
                )

                self.change_state('BACKWARD')

        elif self.state == 'BACKWARD':

            self.publish_backward()

            if elapsed >= self.BACKWARD_TIME:

                self.publish_stop()

                self.change_state('STOP_BEFORE_TURN')

        elif self.state == 'STOP_BEFORE_TURN':

            self.publish_stop()

            if elapsed >= self.STOP_TIME:
                
                self.angle = 0.0
                self.last_gyro_time = None

                self.get_logger().info(
                    '90-Grad-Drehung gestartet'
                )

                self.change_state('TURN')

        elif self.state == 'TURN':

            self.publish_turn_left()

            self.get_logger().info(
                f'Winkel: {self.angle:.1f} deg',
                throttle_duration_sec=0.5
            )

            if abs(self.angle) >= self.TARGET_ANGLE:

                self.publish_stop()

                self.get_logger().info(
                    f'Drehung beendet: '
                    f'{self.angle:.1f} Grad'
                )

                self.change_state('STOP_AFTER_TURN')

        elif self.state == 'STOP_AFTER_TURN':

            self.publish_stop()

            if elapsed >= self.STOP_TIME:

                self.change_state('FORWARD')


def main(args=None):

    rclpy.init(args=args)

    node = ObstacleAvoidance()

    try:
        rclpy.spin(node)

    except KeyboardInterrupt:
        pass

    node.publish_stop()

    node.destroy_node()

    rclpy.shutdown()


if __name__ == '__main__':
    main()