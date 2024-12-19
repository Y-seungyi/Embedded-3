# 금오공대 임베디드 시스템 3조
조원 - 김용승, 김정훈, 정석영, 남현준
<br/><br/>

## 주요 아이디어
스마트폰 앱을 사용해 청소기의 상태를 확인하고 조종, 예약기능을 사용해 원하는 시간대에 원하는 시간만큼 작동하게 하는 로봇 청소기
<br/><br/>

## 전체 시스템 구조

![image](https://github.com/user-attachments/assets/7a5b78ca-096c-4c82-b1bf-10c6b8be8398)
<br/><br/>

## 사용한 센서와 액추에이터
***센서*** 
* ${\large{\color{skyblue}버튼}}$: 청소기에 수동 버튼을 달아 수동으로 상태를 변환
*  ${\large{\color{skyblue}초음파 센서}}$: 초음파 센서를 사용해 앞의 장애물을 감지하면 방향을 전환하도록 설계

***액추에이터***
* ${\large{\color{skyblue}DC모터}}$: DC모터를 사용해 청소기 아래의 청소솔과 바퀴를 조작
* ${\large{\color{skyblue}LED}}$: LED를 사용해 청소기가 작동중인지 확인

***모듈***
* ${\large{\color{skyblue}블루투스 모듈}}$: 블루투스 모듈을 사용해 앱과 원격 통신, 원격으로 on/off 와 원하는 시간 예약
* ${\large{\color{skyblue}RTC 모듈}}$: RTC모듈을 사용해 현재 시간 측정 후 예약한 시간에 맞춰 동작과 정지신호 전달

<br/>

## 주요기능
+ ${\large{\color{skyblue}블루투스 모듈}}$ 을 사용해 스마트폰의 앱과 라즈베리파이를 실시간으로 신호전달
+ ${\large{\color{skyblue}스마트폰 앱의 예약기능}}$을 이용해 원하는 시간대에 작동할 수 있도록 청소시간을 예약
+ ${\large{\color{skyblue}수동 조작 버튼}}$으로 로봇청소기를 수동 on/off
+ ${\large{\color{skyblue}초음파센서}}$ 를 사용해 정면의 장애물을 감지해 장애물이 없는 곳으로 경로 탐색
  <br/><br/>

## 주요 기능 흐름 (쓰레드)
```
typedef struct
{
    int id;
    int day;      // 요일 (1-7, 1=월요일, 7=일요일)
    int hour;     // 시 (24시간 기준)
    int minute;   // 분 (60분 기준)
    int duration; // 초단위
} Schedule;
Schedule schedules[MAX_SCHEDULES]; // 예약된 시간
```
예약되는 시간을 기록하는 구조체
구조체에 요일, 시, 분, 작동할 초를 기록해서 schedules 배열에 기록

<br/>

```
pthread_create(&uart_thread, NULL, perform_uart, NULL); //블루투스 관련 쓰레드
pthread_create(&distance_thread, NULL, perform_distance, NULL); // 초음파 센서 거리 측정 및 바퀴 제어
pthread_create(&schedule_thread, NULL, perform_schedule, NULL); // 예약한 시간 확인
```

3개의 쓰레드를 사용
블루투스, 거리 측정, 예약 확인 쓰레드를 각각 사용
<br/><br/>

### - ${\large{\color{skyblue}블루투스 쓰레드}}$ 주요 명령어 ###
1. **CONNECT** : 블루투스 앱과 연결이 되었을 때 예약된 스케줄을 모두 전송 받아 출력, 청소기의 상태에 따라 켜져있을 때와 꺼져있을 때를 확인해 출력
2. **START** : 청소기와 청소 솔의 시작 신호를 1(ON)로 변경
3. **STOP** : 청소기의 청소 솔의 시작 신호를 0(OFF)으로 변경
4. **STATUS** : 청소기의 상태를 확인해서 전송
5. **ADD_SCHEDULE** : 스케줄을 추가
6. **DELETE_SCHEDULE** : 스케줄을 찾아서 삭제후 남아있는 스케줄을 다시 전송
받아오는 명령어에 따라 역할을 수행
<br/><br/>

### - ${\large{\color{skyblue}거리측정 쓰레드}}$ 주요 기능 ##
1. 초음파센서를 사용해 장애물과의 distance를 파악
2. 임의로 정한 임계값보다 distance가 가까워질 경우 장애물이 감지되었다고 판단
3. 기기를 기준으로 180도 회전 후 직진 (좌, 우 반복해서 기기가 지그재그로 움직이도록 설계)
<br/><br/>

### - ${\large{\color{skyblue}예약 확인 쓰레드}}$ 주요 기능 ###
1. time.h의 함수를 사용해 현재시간 측정(RTC 모듈의 배터리가 없어 대체)
2. 현재의 시간과 RTC 모듈의 시간을 1초마다 탐색하면서 스케줄을 비교
3. 청소기가 켜져 있을 때 스케줄을 비교해 같은 시간이 되었을 때 청소기를 시작
4. 정해 놓은 시간이 초과했거나 수동 종료를 했을 때 청소기 종료
<br/><br/>

## 주요 기능 흐름 (뮤텍스)
- **started_manual** : 예약 확인 쓰레드에서 started_manual의 값을 읽고 판단하는 코드가 존재하기 때문에 읽고 있는 도중에 값이 변할 경우 문제가 발생할 수 있음
- **cleaning_status** : 블루투스로 on/off 를 하거나, 수동으로 on/off를 할 때 중복이 발생할 수 있음
- **schedules 사용** : 예약 확인 쓰레드에서 1초마다 시간과 스케줄을 비교함. 따라서 스케줄이 변경(추가 or 삭제)될 때, 스케줄을 읽을 때 충돌이 발생할 수 있음
<br/><br/>

## 데모 영상
![수동시작](https://github.com/user-attachments/assets/7475fdd2-8b62-4c4d-b7cb-5234d4241162)
- 수동 버튼을 눌렀을 때 바퀴와 청소 솔 작동
  <br/><br/><br/><br/>
  

![장애물+탐색](https://github.com/user-attachments/assets/0a35b9c1-b83c-42d9-8ead-d2152a59d971)
- 장애물을 만났을 때 회전
  <br/><br/><br/><br/>
  

![원격+시작](https://github.com/user-attachments/assets/2276e111-74f8-4f76-be8f-ca20f238a435)
- 블루투스 앱을 사용해 원격 조작
  <br/><br/><br/><br/>
  
![예약+시작2](https://github.com/user-attachments/assets/8daff494-e000-4d45-ad14-769881c40ab3)
- 블루투스 앱 예약과 예약 시간이 되었을 때 시작
   <br/><br/><br/><br/>

전체 데모영상 링크 https://www.youtube.com/shorts/5vVVJBEgOeA
  <br/><br/>
  
## 문제 및 해결방안
1. 바퀴 두개의 속도가 서로 다른 문제점 발생 -> 두바퀴의 속도를 pwm을 사용해 같은 방향으로 가게끔 제어
2. 수동으로 전원을 켰을 때 작동이 바로 멈추는 문제 발생 -> started_manual 변수를 추가해 수동 작동시 바로 꺼지지 않게 수정
3. DC모터의 vcc와 gnd를 반대로 꽂아 회로가 타는 문제 발생 -> 칩을 다른 DC모터와 교환해 정상 작동
<br/><br/>

## 한계점
- DC모터 하나만으로 정확한 방향을 구현하기가 어려움 -> 다른 센서를 구입하지 못한 아쉬움
