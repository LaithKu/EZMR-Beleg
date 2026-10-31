import rclpy
from rclpy.node import Node
from geometry_msgs.msg import Twist


class ShapeController(Node):

    def __init__(self):
        super().__init__('shape_controller')

        self.publisher_ = self.create_publisher(
            Twist,
            '/cmd_vel',
            10
        )

        self.timer_period = 0.1
        self.timer = self.create_timer(
            self.timer_period,
            self.timer_callback
        )


        self.state = 0

        self.counter = 0

        self.side = 0

        self.get_logger().info('Shape Controller gestartet')


    def timer_callback(self):

        msg = Twist()


        if self.state == 0:

            msg.linear.x = 1.0
            msg.angular.z = 0.0

            self.counter += 1

            if self.counter >= 20:

                self.counter = 0
                self.state = 1
                
        elif self.state == 1:

            msg.linear.x = 0.0
            msg.angular.z = 1.57

            self.counter += 1
            
            if self.counter >= 10:

                self.counter = 0
                self.state = 0

                self.side += 1

                self.get_logger().info(
                    f'Seite {self.side} fertig'
                )

        if self.side >= 4:

            msg.linear.x = 0.0
            msg.angular.z = 0.0

        self.publisher_.publish(msg)


def main(args=None):

    rclpy.init(args=args)

    node = ShapeController()

    rclpy.spin(node)

    node.destroy_node()

    rclpy.shutdown()


if __name__ == '__main__':
    main()