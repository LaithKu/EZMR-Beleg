#!/usr/bin/env python3

import math

import rclpy
from rclpy.node import Node

from sensor_msgs.msg import LaserScan
from geometry_msgs.msg import Twist


class WallFollower(Node):

    def __init__(self):
        super().__init__('wall_follower')

        # ---------------------------------------------------------
        # EINSTELLUNGEN
        # ---------------------------------------------------------

        # Zielabstand zur Wand in Metern
        self.target_distance = 0.40

        # Sicherheitsabstand zur Wand.
        # Unter 50 cm soll der Roboter auf keinen Fall fahren.
        self.min_distance = 0.30

        # Geschwindigkeit
        self.forward_speed = 0.5       # m/s
        self.turn_speed = 0.4           # rad/s

        # Wie stark auf den Abstand zur Wand reagiert wird
        self.kp = 1.0

        # Maximale Lenkgeschwindigkeit
        self.max_angular_speed = 0.4

        # Abstand, ab dem ein Hindernis vorne erkannt wird
        self.front_obstacle_distance = 0.5

        # ---------------------------------------------------------
        # ROS
        # ---------------------------------------------------------

        self.scan_subscriber = self.create_subscription(
            LaserScan,
            '/scan',
            self.scan_callback,
            10
        )

        self.cmd_publisher = self.create_publisher(
            Twist,
            '/cmd_vel',
            10
        )

        # Sicherheits-Timer:
        # Wenn keine neuen Laserdaten kommen, wird gestoppt.
        self.timer = self.create_timer(
            0.1,
            self.control_loop
        )

        self.latest_scan = None

        self.get_logger().info('Wall Follower gestartet.')
        self.get_logger().info(
            f'Zielabstand: {self.target_distance:.2f} m'
        )

    # ---------------------------------------------------------
    # LASERDATEN EMPFANGEN
    # ---------------------------------------------------------

    def scan_callback(self, msg):
        self.latest_scan = msg

    # ---------------------------------------------------------
    # EINEN BEREICH DES LASERSCANS AUSWERTEN
    # ---------------------------------------------------------

    def get_distance(self, scan, angle_deg):
        """
        Liefert die kleinste gültige Entfernung
        in einem kleinen Winkelbereich um angle_deg.
        """

        angle_rad = math.radians(angle_deg)

        # ±5 Grad um den gewünschten Winkel
        angle_min = angle_rad - math.radians(20)
        angle_max = angle_rad + math.radians(20)

        index_min = int(
            (angle_min - scan.angle_min) / scan.angle_increment
        )

        index_max = int(
            (angle_max - scan.angle_min) / scan.angle_increment
        )

        index_min = max(0, index_min)
        index_max = min(len(scan.ranges) - 1, index_max)

        distances = []

        for i in range(index_min, index_max + 1):

            distance = scan.ranges[i]

            if math.isfinite(distance):
                if scan.range_min <= distance <= scan.range_max:
                    distances.append(distance)

        if not distances:
            return float('inf')

        return min(distances)

    # ---------------------------------------------------------
    # HAUPTREGELUNG
    # ---------------------------------------------------------

    def control_loop(self):

        # Noch keine Laserdaten erhalten
        if self.latest_scan is None:
            self.stop_robot()
            return

        scan = self.latest_scan

        # -----------------------------------------------------
        # MESSWERTE
        # -----------------------------------------------------

        # Rechts
        right_distance = self.get_distance(scan, 180)

        # Vorne rechts
        front_right_distance = self.get_distance(scan, -125)

        # Vorne
        front_distance = self.get_distance(scan, -90)

        # -----------------------------------------------------
        # TWIST-NACHRICHT
        # -----------------------------------------------------

        cmd = Twist()

        # -----------------------------------------------------
        # FALL 1:
        # Hindernis direkt vorne
        # -----------------------------------------------------

        if front_distance < self.front_obstacle_distance:

            self.get_logger().info(
                f'Hindernis vorne: {front_distance:.2f} m - '
                'drehe links'
            )

            cmd.linear.x = 0.0
            cmd.angular.z = self.turn_speed

            self.cmd_publisher.publish(cmd)
            return

        # -----------------------------------------------------
        # FALL 2:
        # Wand ist sehr nah
        # -----------------------------------------------------

        if right_distance < self.min_distance:

            # Wir sind zu nah an der Wand.
            # Deshalb nach links drehen.
            
            self.get_logger().info(
                f'Wand zu nah: {right_distance:.2f} m - '
                'drehe links'
            )

            error = self.target_distance - right_distance

            angular =  self.kp * error

            angular = max(
                - self.max_angular_speed,
                min(self.max_angular_speed, angular)
            )

            cmd.linear.x = self.forward_speed * 0.6
            cmd.angular.z = angular

        # -----------------------------------------------------
        # FALL 3:
        # Wand ist weiter entfernt
        # -----------------------------------------------------

        else:

            # Fehler:
            #
            # positive Zahl = Wand zu weit weg

            error = right_distance - self.target_distance
            
            self.get_logger().info(
                f'Wand zu weit: {right_distance:.2f} m - '
                'drehe rechts'
            )

            # Bei einer rechten Wand:
            #
            # Wand zu weit weg  -> nach rechts drehen
 

            angular = - self.kp * error

            angular = max(
                - self.max_angular_speed,
                min(self.max_angular_speed, angular)
            )

            cmd.linear.x = self.forward_speed
            cmd.angular.z = angular

        # -----------------------------------------------------
        # VERÖFFENTLICHEN
        # -----------------------------------------------------

        self.cmd_publisher.publish(cmd)

    # ---------------------------------------------------------
    # ROBOTER STOPPEN
    # ---------------------------------------------------------

    def stop_robot(self):

        cmd = Twist()

        cmd.linear.x = 0.0
        cmd.linear.y = 0.0
        cmd.linear.z = 0.0

        cmd.angular.x = 0.0
        cmd.angular.y = 0.0
        cmd.angular.z = 0.0

        self.cmd_publisher.publish(cmd)


def main(args=None):

    rclpy.init(args=args)

    node = WallFollower()

    try:
        rclpy.spin(node)

    except KeyboardInterrupt:
        pass

    finally:
        node.stop_robot()
        node.destroy_node()
        rclpy.shutdown()


if __name__ == '__main__':
    main()
