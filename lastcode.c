#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <wiringPi.h>
#include <wiringSerial.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <time.h>
#include <wiringPiI2C.h>

#define MOTOR_A1A 18 // 좌측 모터 방향 제어 핀
#define MOTOR_A1B 17
#define MOTOR_B1A 20 // 우측 모터 방향 제어 핀
#define MOTOR_B1B 21

#define MAX_ROTATIONS 3 // 회전 횟수 제한

#define TRIG_PIN 23             // 초음파 센서 트리거 핀
#define ECHO_PIN 24             // 초음파 센서 에코 핀
#define DISTANCE_THRESHOLD 15.0 // 장애물 감지 거리 임계값

#define BAUD_RATE 115200                       // 시리얼 통신 속도
static const char *UART2_DEV = "/dev/ttyAMA2"; // UART 포트

#define I2C_DEV "/dev/i2c-1" // I2C 장치 파일
#define SLAVE_ADDR_01 0x68   // RTC I2C 주소
#define MAX_SCHEDULES 10     // 최대 예약 수

#define BRUSH_PWM 16
#define BRUSH_PWM2 19

#define FORWARD 1
#define BACKWARD 2
#define TURN_LEFT 3
#define TURN_RIGHT 4
#define STOP 0

// 예약된 시간 배열
typedef struct
{
    int id;
    int day;      // 요일 (1-7, 1=월요일, 7=일요일)
    int hour;     // 시 (24시간 기준)
    int minute;   // 분 (60분 기준)
    int duration; // 초단위
} Schedule;
Schedule schedules[MAX_SCHEDULES]; // 예약된 시간

int schedule_count = 0; // 예약된 시간 수
char *k_day[] = {"x", "월", "화", "수", "목", "금", "토", "일"};

// mutex
pthread_mutex_t mutex, mutex2;

Schedule schedule_info; // 청소 예약 정보

// 전역 변수
int fd_serial;
int rotation_count = 0;
int turn_direction = 4;         // 기본 회전 방향: 우측
volatile sig_atomic_t stop = 0; // 프로그램 종료 플래그
int cleaning_status = 0;        // 청소 상태 (0: Idle, 1: Working)
int current_cleaning_duration = 0;
int started_manual = 0;

// 함수 선언
void initMovingMotor();
void initPwm();
void moveMotor(int dir, float speed);
void stopMotor();
float measureDistance();
unsigned char serialRead(const int fd);
void serialWriteBytes(const int fd, const char *s);
void sendStatus(const char *status);
void handle_command(const char *command);
void *perform_uart(void *arg);
void *perform_distance(void *arg);
void handle_signal(int sig);
int add_schedule();
void delete_schedule();
void check_schedules(int i2c_fd, int day, int hour, int minute, int second);
void initialize_motors();
void set_brush_motor(int bool);

// 바퀴 제어 관련 함수들
void initMovingMotor()
{
    pinMode(MOTOR_A1A, OUTPUT);
    pinMode(MOTOR_A1B, OUTPUT);
    pinMode(MOTOR_B1A, OUTPUT);
    pinMode(MOTOR_B1B, OUTPUT);
    softPwmCreate(MOTOR_A1A, 0, 1024);
    softPwmCreate(MOTOR_A1B, 0, 1024);
    softPwmCreate(MOTOR_B1A, 0, 1024);
    softPwmCreate(MOTOR_B1B, 0, 1024);
}

void initPwm()
{
    int hz = 1000;
    int range = 1024;
    int divisor = 19200000 / hz / range;
    pwmSetMode(PWM_MODE_MS);
    pwmSetRange(range);
    pwmSetClock(divisor);
}

void initSonar()
{
    pinMode(ECHO_PIN, INPUT);
    pinMode(TRIG_PIN, OUTPUT);
}

void moveMotor(int dir, float speed)
{
    int rightSpeed = 750 * speed;
    int leftSpeed = 550 * speed;
    switch (dir)
    {
    case 1: // FORWARD
        softPwmWrite(MOTOR_A1A, rightSpeed);
        softPwmWrite(MOTOR_A1B, 0);
        softPwmWrite(MOTOR_B1A, 0);
        softPwmWrite(MOTOR_B1B, leftSpeed);
        break;
    case 2: // BACKWARD
        softPwmWrite(MOTOR_A1A, 0);
        softPwmWrite(MOTOR_A1B, rightSpeed);
        softPwmWrite(MOTOR_B1A, leftSpeed);
        softPwmWrite(MOTOR_B1B, 0);
        break;
    case 3: // TURN_LEFT
        softPwmWrite(MOTOR_A1A, 0);
        softPwmWrite(MOTOR_A1B, rightSpeed);
        softPwmWrite(MOTOR_B1A, 0);
        softPwmWrite(MOTOR_B1B, leftSpeed);
        break;
    case 4: // TURN_RIGHT
        softPwmWrite(MOTOR_A1A, rightSpeed);
        softPwmWrite(MOTOR_A1B, 0);
        softPwmWrite(MOTOR_B1A, leftSpeed);
        softPwmWrite(MOTOR_B1B, 0);
        break;
    case 0: // STOP
    default:
        softPwmWrite(MOTOR_A1A, 0);
        softPwmWrite(MOTOR_A1B, 0);
        softPwmWrite(MOTOR_B1A, 0);
        softPwmWrite(MOTOR_B1B, 0);
        break;
    }
}

void stopMotor()
{
    moveMotor(0, 0.0);
}

float measureDistance()
{
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);

    unsigned long start_time = micros();
    while (digitalRead(ECHO_PIN) == LOW)
    {
        if (micros() - start_time > 3000000)
            return -1.0;
    }

    start_time = micros();
    while (digitalRead(ECHO_PIN) == HIGH)
    {
        if (micros() - start_time > 3000000)
            break;
    }

    unsigned long end_time = micros();
    return (end_time - start_time) / 29.0 / 2.0;
}

// 시리얼 통신 관련 함수들
unsigned char serialRead(const int fd)
{
    unsigned char x;
    if (read(fd, &x, 1) != 1)
        return -1;
    return x;
}

void serialWriteBytes(const int fd, const char *s)
{
    write(fd, s, strlen(s));
}

void sendStatus(const char *status)
{
    serialWriteBytes(fd_serial, status);
    serialWriteBytes(fd_serial, "\n");
}

void handle_command(const char *command)
{
    if (strcmp(command, "CONNECT") == 0)
    {
        send_all_schedule();
        send_cleaning_status();
    }
    else if (strcmp(command, "START") == 0)
    {
        start_clean();
        pthread_mutex_lock(&mutex2);
        started_manual = 1;
        pthread_mutex_unlock(&mutex2);
    }
    else if (strcmp(command, "STOP") == 0)
    {
        stop_clean();
        pthread_mutex_lock(&mutex2);
        started_manual = 0;
        pthread_mutex_unlock(&mutex2);
    }
    else if (strcmp(command, "STATUS") == 0)
    {
        send_cleaning_status();
    }
    else if (strncmp(command, "ADD_SCHEDULE", 12) == 0)
    {
        int id, day, hour, minute, second, duration;
        if (sscanf(command, "ADD_SCHEDULE %d %d-%d:%d:%d %d", &id, &day, &hour, &minute, &second, &duration) == 6)
        {
            schedule_info.id = id;
            schedule_info.day = day;
            schedule_info.hour = hour;
            schedule_info.minute = minute;
            schedule_info.duration = duration;
            if (add_schedule())
            {
                send_schedule(id);
            }
        }
    }
    else if (strncmp(command, "DELETE_SCHEDULE", 15) == 0)
    {
        int id;
        if (sscanf(command, "DELETE_SCHEDULE %d", &id) == 1)
        {
            pthread_mutex_lock(&mutex);
            schedule_info.id = id;
            pthread_mutex_unlock(&mutex);
            delete_schedule();
            send_all_schedule();
        }
    }
}

void send_cleaning_status()
{
    sendStatus(cleaning_status ? "STATUS WORKING" : "STATUS IDLE");
    printf("청소 상태: %s\n", cleaning_status ? "WORKING" : "IDLE");
}

void send_schedule(int ind)
{
    char schedule_str[50];
    snprintf(schedule_str, sizeof(schedule_str), "SCHEDULE %d %d-%d:%d:%d %d",
             ind,
             schedules[ind].day,
             schedules[ind].hour,
             schedules[ind].minute,
             0, // seconds
             schedules[ind].duration);
    sendStatus(schedule_str);
}

void send_all_schedule()
{
    pthread_mutex_lock(&mutex);
    for (int i = 0; i < schedule_count; i++)
    {
        send_schedule(i);
    }
    char count_str[16];
    sprintf(count_str, "SCHEDULE_COUNT %d", schedule_count);
    pthread_mutex_unlock(&mutex);
    sendStatus(count_str);
}

// 예약 관련 함수들
int to_bcd(int decimal)
{
    return ((decimal / 10) << 4) | (decimal % 10);
}

int from_bcd(int hex)
{
    return (hex >> 4) * 10 + (hex & 0x0F);
}

int add_schedule()
{
    pthread_mutex_lock(&mutex);
    if (schedule_count >= MAX_SCHEDULES)
    {
        return 0;
    }
    schedules[schedule_info.id].day = schedule_info.day;
    schedules[schedule_info.id].hour = schedule_info.hour;
    schedules[schedule_info.id].minute = schedule_info.minute;
    schedules[schedule_info.id].duration = schedule_info.duration;
    printf("예약 추가: %d-%d:%d %d\n", schedules[schedule_info.id].day, schedules[schedule_info.id].hour, schedules[schedule_info.id].minute, schedules[schedule_info.id].duration);

    if (schedule_info.id == schedule_count)
        schedule_count++;
    pthread_mutex_unlock(&mutex);
    return 1;
}

void delete_schedule()
{
    pthread_mutex_lock(&mutex);
    if (schedule_count == 0 || schedule_info.id < 0 || schedule_info.id >= schedule_count)
    {
        pthread_mutex_unlock(&mutex);
        return;
    }

    for (int j = schedule_info.id; j < schedule_count - 1; j++)
    {
        schedules[j] = schedules[j + 1];
    }
    schedule_count--;
    pthread_mutex_unlock(&mutex);
}

// RTC에서 시간을 읽어 예약된 시간과 비교
void check_schedules(int i2c_fd, int day, int hour, int minute, int second)
{
    pthread_mutex_lock(&mutex);
    // 예약된 시간과 비교
    for (int i = 0; i < schedule_count; i++)
    {
        if (schedules[i].day == day && schedules[i].hour == hour && schedules[i].minute == minute && second == 0)
        {
            current_cleaning_duration = schedules[i].duration;
            start_clean();
        }
    }
    pthread_mutex_unlock(&mutex);
}

void initialize_brush_motors()
{
    // 청소 솔 모터 초기화
    pinMode(BRUSH_PWM, OUTPUT);
    pinMode(BRUSH_PWM2, OUTPUT);
    softPwmCreate(BRUSH_PWM, 0, 100);
    softPwmCreate(BRUSH_PWM2, 0, 100);
}

void set_brush_motor(int bool)
{
    if (bool)
    {
        softPwmWrite(BRUSH_PWM, 0);
        softPwmWrite(BRUSH_PWM2, 20);
    }
    else
    {
        softPwmWrite(BRUSH_PWM, 0);
        softPwmWrite(BRUSH_PWM2, 0);
    }
}

void start_clean()
{
    pthread_mutex_lock(&mutex2);
    cleaning_status = 1;
    pthread_mutex_unlock(&mutex2);
    send_cleaning_status();
    set_brush_motor(1);
}

void stop_clean()
{
    pthread_mutex_lock(&mutex2);
    cleaning_status = 0;
    pthread_mutex_unlock(&mutex2);
    send_cleaning_status();
    set_brush_motor(0);
}

// 쓰레드 함수들
void *perform_uart(void *arg)
{
    fd_serial = serialOpen(UART2_DEV, BAUD_RATE);
    if (fd_serial < 0)
    {
        fprintf(stderr, "시리얼 포트 열기 실패\n");
        return NULL;
    }

    char command[128];
    int idx = 0;
    while (1)
    {
        if (serialDataAvail(fd_serial))
        {
            unsigned char dat = serialRead(fd_serial);
            if (dat != (unsigned char)-1)
            {
                if (dat == '\n')
                {
                    command[idx] = '\0';     // 명령어 종료
                    handle_command(command); // 명령어 처리
                    idx = 0;                 // 버퍼 초기화
                }
                else
                {
                    command[idx++] = dat;
                    if (idx >= sizeof(command) - 1)
                    {
                        idx = 0; // 버퍼 크기 초과 방지
                    }
                }
            }
        }
        delay(10);
    }
}

void *perform_distance(void *arg)
{
    int rotation_time = 800000;
    while (1)
    {
        if (cleaning_status)
        {
            printf("청소 중...\n");
            moveMotor(FORWARD, 1.0); // 전진

            float distance = measureDistance();
            if (distance < DISTANCE_THRESHOLD)
            {
                printf("장애물 감지! 거리: %.2f cm\n", distance);
                stopMotor();
                usleep(300000); // 잠시 정지
                if (rotation_count % 2 == 0)
                {
                    moveMotor(TURN_RIGHT, 1.0); // 우회전
                }
                else
                {
                    moveMotor(TURN_LEFT, 1.0); // 좌회전
                }
                usleep(rotation_time); // 잠시 회전
                stopMotor();
                usleep(1000000);
                moveMotor(FORWARD, 1.0);
                usleep(400000);
                if (rotation_count % 2 == 0)
                {
                    moveMotor(TURN_RIGHT, 1.0); // 우회전
                }
                else
                {
                    moveMotor(TURN_LEFT, 1.0); // 좌회전
                }
                usleep(rotation_time); // 잠시 회전
                stopMotor();
                usleep(1000000);
                moveMotor(FORWARD, 1.0);
                usleep(300000);
                rotation_count++;
            }
            usleep(300000);
        }
        else
        {
            stopMotor();
            usleep(100000);
        }
    }
}

void *perform_schedule(void *arg)
{
    int i2c_fd;

    i2c_fd = wiringPiI2CSetupInterface(I2C_DEV, SLAVE_ADDR_01);
    if (i2c_fd == -1)
    {
        printf("I2C 초기화 실패\n");
        return;
    }

    // 현재 시간으로 초기화
    timer_t timer = time(NULL);
    struct tm *t = localtime(&timer);
    wiringPiI2CWriteReg8(i2c_fd, 0x00, to_bcd(t->tm_sec));                    // 초
    wiringPiI2CWriteReg8(i2c_fd, 0x01, to_bcd(t->tm_min));                    // 분
    wiringPiI2CWriteReg8(i2c_fd, 0x02, to_bcd(t->tm_hour));                   // 시
    wiringPiI2CWriteReg8(i2c_fd, 0x03, to_bcd((((t->tm_wday) + 6) % 7) + 1)); // 요일

    int second; // 초
    int minute; // 분
    int hour;   // 시
    int day;    // 요일

    int startTime;
    int _cleaning_status = 0;
    while (1)
    {
        pthread_mutex_lock(&mutex);
        _cleaning_status = cleaning_status;
        pthread_mutex_unlock(&mutex);
        if (!_cleaning_status)
        {
            // 청소 X일 시 스케쥴 탐색
            second = from_bcd(wiringPiI2CReadReg8(i2c_fd, 0x00)); // 초
            minute = from_bcd(wiringPiI2CReadReg8(i2c_fd, 0x01)); // 분
            hour = from_bcd(wiringPiI2CReadReg8(i2c_fd, 0x02));   // 시
            day = from_bcd(wiringPiI2CReadReg8(i2c_fd, 0x03));    // 요일
            printf("현재 시간: %d시 %d분 %d초\n", hour, minute, second);

            check_schedules(i2c_fd, day, hour, minute, second);
            startTime = (hour * 3600) + (minute * 60);

            delay(1000); // 1초 대기
            // system("clear");
        }
        else
        {
            int tempSecond = from_bcd(wiringPiI2CReadReg8(i2c_fd, 0x00)); // 초
            int tempMinute = from_bcd(wiringPiI2CReadReg8(i2c_fd, 0x01)); // 분
            int tempHour = from_bcd(wiringPiI2CReadReg8(i2c_fd, 0x02));   // 시
            int tempDay = from_bcd(wiringPiI2CReadReg8(i2c_fd, 0x03));    // 요일
            printf("현재 시간: %d시 %d분 %d초\n", tempHour, tempMinute, tempSecond);

            // 현재 시간 계산 (초 단위로)
            int curTime = (tempHour * 3600) + (tempMinute * 60) + tempSecond;

            // 24시간을 넘어갈 경우 계산
            if (tempDay != day)
            {
                curTime += 24 * 3600; // 하루를 더해서 경과 시간을 맞추기
            }

            pthread_mutex_lock(&mutex);
            // 경과 시간이 청소 예상 시간을 초과했으면
            if (curTime - startTime > current_cleaning_duration && started_manual == 0)
            {
                // mutex
                stop_clean();
            }
            pthread_mutex_unlock(&mutex);
        }
    }

    return 0;
}

int main()
{
    pthread_mutex_init(&mutex, NULL);

    wiringPiSetupGpio();
    initMovingMotor();
    initialize_brush_motors();
    initPwm();
    initSonar();

    pthread_t uart_thread;
    pthread_t distance_thread;
    pthread_t schedule_thread;

    // 쓰레드 시작
    pthread_create(&uart_thread, NULL, perform_uart, NULL);
    pthread_create(&distance_thread, NULL, perform_distance, NULL);
    pthread_create(&schedule_thread, NULL, perform_schedule, NULL);

    // 시그널 처리
    // signal(SIGINT, handle_signal);

    // 프로그램 종료까지 대기
    pthread_join(uart_thread, NULL);
    pthread_join(distance_thread, NULL);
    pthread_join(schedule_thread, NULL);

    pthread_mutex_destroy(&mutex);

    return 0;
}
