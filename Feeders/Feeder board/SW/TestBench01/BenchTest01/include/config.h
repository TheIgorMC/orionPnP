#ifndef CONFIG_H
#define CONFIG_H

// Motor A (FEED)
#define MOTOR_A_PWM 3   //AIN1
#define MOTOR_A_DIR 9   //AIN2

// Motor B (PEEL)
#define MOTOR_B_PWM 6   //BIN1
#define MOTOR_B_DIR 10   //BIN2

#define SLEEP_DRV A0

// Default test values
#define DEFAULT_PWM 180        // 0-255
#define DEFAULT_ON_TIME 100    // milliseconds
#define DEFAULT_OFF_TIME 500

#endif
