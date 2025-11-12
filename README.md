# Pintos: 우선순위 스케줄링 구현

Pintos 교육용 운영체제에서 우선순위 기반 스레드 스케줄링을 시스템 수준으로 구현한 프로젝트입니다.

## 개요

이 프로젝트에서는 우선순위 역전(priority inversion)을 방지하기 위한 우선순위 기부(priority donation) 메커니즘과 세마포어/락 관리를 통해, 교착 상태(deadlock)나 기아(starvation) 없이 복잡한 동시성 작업을 구현했습니다

## 주요 기능

### 우선순위 스케줄링
- **우선순위 기반 준비 큐**: 준비 상태 스레드를 우선순위 순서로 관리하여 높은 우선순위 스레드부터 실행되도록 보장
- **동적 우선순위 선점**: 높은 우선순위 스레드가 준비 상태가 되면 현재 실행 중인 낮은 우선순위 스레드를 즉시 선점
- **최적화된 리스트 관리**:
  - **ready_list**: 스레드 생성 시 `list_insert_ordered()` 사용으로 정렬 유지
  - **세마포어 대기열**: `sema_down()`에서 `list_push_back()` 사용 후 `sema_up()` 시점에만 정렬하여 불필요한 정렬 연산 최소화
  - **조건 변수 대기열**: `cond_wait()`에서 `list_push_back()` 사용 후 `cond_signal()` 시점에 `list_sort()` 호출로 최고 우선순위 스레드 선택

### 우선순위 기부 (우선순위 역전 방지)
- **중첩 기부 지원**: 최대 8단계 깊이의 락 체인을 처리하여 우선순위를 보유-대기 관계로 전파
- **선택적 기부 제거**: 락 해제 시 해당 락과 관련된 기부만 제거하며, 다른 락으로부터의 기부는 유지
- **원래 우선순위 보존**: 스레드가 `priority`(기부받은 후의 우선순위)와 `original_priority`(기본 우선순위)를 모두 유지하여 우선순위 복원 가능
- **동적 ready_list 재정렬**:
  - `donate_priority()`: 우선순위 기부 시 holder가 READY 상태이면 ready_list에서 제거 후 재삽입하여 정렬 유지
  - `recalculate_priority()`: 락 해제 후 우선순위 재계산 시 변경되었으면 ready_list 재정렬



## 아키텍처

### 스레드 구조
```c
struct thread {
    int priority;                    // 현재 효과적 우선순위
    int original_priority;           // 기부 이전의 기본 우선순위
    struct list donators;            // 이 스레드에게 기부한 스레드들
    struct lock *waiting_lock;       // 이 스레드가 대기 중인 락
    struct list_elem donation_elem;  // donators 리스트용 요소
    // ... 다른 필드들
};
```

### 락과 세마포어 설계
```
락 (이진 자원)
├── holder: 현재 소유 스레드
└── semaphore: 접근 제어 (값: 0 또는 1)
    ├── value: 사용 가능한 개수
    └── waiters: 대기 스레드 리스트 (sema_up() 시점에 정렬)

조건 변수
└── waiters: semaphore_elem 구조체 리스트 (cond_signal() 시점에 정렬)
    └── 각 요소는 스레드 조정용 개별 세마포어 포함

ready_list (전역 변수)
└── thread.h에 extern 선언되어 다른 모듈에서 접근 가능
```
## 핵심 함수

| 함수 | 목적 |
|------|------|
| `sema_down()` | waiters에 `list_push_back()`으로 스레드 추가 후 블록 |
| `sema_up()` | waiters를 정렬 후 최고 우선순위 스레드를 깨움 |
| `lock_acquire()` | 우선순위 기부를 지원하는 락 획득 |
| `lock_release()` | 락 해제 및 우선순위 재계산 |
| `donate_priority()` | 락 체인을 통한 우선순위 전파 + READY 상태 스레드 ready_list 재정렬 |
| `recalculate_priority()` | donators 리스트 기반 우선순위 재계산 + ready_list 재정렬 |
| `remove_donations()` | 특정 락과 관련된 기부만 선택적으로 제거 |
| `cond_wait()` | waiters에 `list_push_back()`으로 추가 후 대기 |
| `cond_signal()` | waiters 정렬 후 최고 우선순위 스레드 깨우기 |
| `preemption_by_priority()` | 선점 필요 여부 확인 및 수행 |
| `compare_ready_priority()` | 스레드 우선순위 비교 함수 (내림차순) |
| `compare_donation_priority()` | donators 리스트 정렬용 비교 함수 |

## 사용 예제

```c
// 동기화 원시 요소 초기화
struct lock resource_lock;
struct condition data_ready;
lock_init(&resource_lock);
cond_init(&data_ready);

// 자동 우선순위 기부를 지원하는 임계 섹션
lock_acquire(&resource_lock);
// ... 보호된 코드 ...
if (condition_met) {
    cond_signal(&data_ready, &resource_lock);
}
lock_release(&resource_lock);

// 조건 변수를 이용한 대기
lock_acquire(&resource_lock);
while (!condition_met) {
    cond_wait(&data_ready, &resource_lock);
}
// ... 보호된 데이터로 진행 ...
lock_release(&resource_lock);
```

## 성능 특성

- **락 획득**: O(n) donators 리스트 삽입
- **락 해제**: O(n) 기부 제거 + O(n) ready_list 재정렬
- **세마포어 연산**:
  - `sema_down()`: O(1) list_push_back
  - `sema_up()`: O(n log n) 정렬 후 O(1) unblock
- **우선순위 기부**:
  - `donate_priority()`: O(depth × n) 체인 전파 + ready_list 재정렬
  - `recalculate_priority()`: O(n) donators 탐색 + ready_list 재정렬
- **조건 변수**:
  - `cond_wait()`: O(1) list_push_back
  - `cond_signal()`: O(n log n) 정렬 후 sema_up
- **우선순위 선점**: O(1) 확인 (ready_list는 정렬 유지)

## 테스트

다음을 포함하는 우선순위 스케줄링 테스트 케이스를 통과합니다:
- 준비 큐에서의 우선순위 순서 지정
- 우선순위 역전 방지 우선순위 기부
- 깊은 기부 전파를 지원하는 중첩 락 체인
- 조건 변수 우선순위 의미론
- 올바른 우선순위 복원을 지원하는 락 해제

## 설계 결정

1. **지연 정렬 (Lazy Sorting) 최적화**:
   - **세마포어/조건 변수**: 대기열 삽입 시 `list_push_back()` O(1) 사용, 깨울 때만 정렬
   - **ready_list**: 항상 정렬 유지 (`list_insert_ordered()` 사용)
   - **이유**: 세마포어는 여러 스레드가 대기하다가 한 번에 하나씩만 깨우므로, 삽입마다 정렬하는 것보다 깨울 때 한 번만 정렬하는 것이 효율적

2. **ready_list 동적 재정렬**:
   - 우선순위 기부/재계산 시 READY 상태 스레드의 ready_list 위치를 동적으로 재조정
   - 우선순위 변경 즉시 스케줄링 순서에 반영하여 정확한 우선순위 스케줄링 보장

3. **선택적 기부 제거**:
   - 특정 락과 관련된 기부만 제거하여 스레드가 독립적인 우선순위 기부를 지원하는 다중 락 보유 가능
   - `remove_donations()`에서 `waiting_lock` 필드로 기부자 필터링

4. **최대 깊이 제한**:
   - 중첩 기부를 8단계로 제한하여 무한 루프 방지 및 현실적 락 계층 구조 지원

5. **Mesa vs. Hoare 의미론**:
   - 조건 변수가 Mesa 스타일 의미론(원자적이지 않은 신호/대기) 사용으로 깨어남 후 조건 재확인 요구

## 파일 구성

### 핵심 구현 파일
- `pintos/include/threads/synch.h` - 동기화 원시 요소 헤더
- `pintos/threads/synch.c` - 세마포어, 락, 조건 변수 구현
- `pintos/include/threads/thread.h` - 스레드 구조체, ready_list extern 선언, 우선순위 함수 선언
- `pintos/threads/thread.c` - 스레드 초기화, 스케줄링, ready_list 정의

### 리스트 라이브러리
- `pintos/include/lib/kernel/list.h` - 리스트 자료구조 인터페이스
- `pintos/lib/kernel/list.c` - 리스트 구현, 성능 측정 함수 포함

### 테스트 파일
- `pintos/tests/threads/priority-donate-one.c` - 단일 락 우선순위 기부 테스트
- `pintos/tests/threads/priority-donate-multiple.c` - 다중 락 우선순위 기부 테스트
- `pintos/tests/threads/priority-donate-sema.c` - 세마포어 우선순위 기부 테스트

## 주요 변경 이력

### 최근 최적화 (2025-11-13)
- **리스트 정렬 최적화**: 세마포어/조건 변수 대기열에서 지연 정렬 기법 적용
  - `sema_down()`: `list_insert_ordered()` → `list_push_back()`
  - `cond_wait()`: `list_insert_ordered()` → `list_push_back()`
- **ready_list 외부 접근**: `thread.c` → `thread.h`로 선언 이동하여 `synch.c`에서 접근 가능
- **동적 재정렬 추가**:
  - `donate_priority()`: holder가 READY 상태일 때 ready_list 재정렬
  - `recalculate_priority()`: 우선순위 변경 시 ready_list 재정렬
- **변수명 개선**: `top_donor` → `top_donator` 일관성 향상

## 향후 개선 사항

- 락 경합(lock contention) 시나리오 추가 성능 최적화
- ready_list 재정렬 빈도 최소화 방안 검토

## 참고 자료
Pintos 공식 문서

