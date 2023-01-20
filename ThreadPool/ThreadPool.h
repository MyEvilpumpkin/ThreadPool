/// \file
/// \brief Файл с описанием класса ThreadPool
/// \details Данный файл содержит в себе описание класса для работы с пулом потоков

#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <type_traits>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <queue>
#include <unordered_map>
#include <any>
#include <optional>
#include <functional>

namespace mep {

	/// \brief Класс ThreadPool
	/// \details Класс - описание пула потоков
	class ThreadPool {
	public:
		/// Определение типа - ID задачи
		using TaskId = std::optional<size_t>;

		/// Перечисление возможных статусов задачи
		enum class TaskStatus {
			CREATED,	///< Задача создана и добавлена в очередь
			STARTED,	///< Задача выполняется
			COMPLETED,	///< Задача выполнена
			CANCELED	///< Задача отменена
		};

		/// \brief Структура TaskInfo
		/// \details Структура - описание информации о задаче
		struct TaskInfo {
			TaskStatus status;	///< Статус задачи
			std::any result;	///< Результат выполнения задачи
		};

		/// Перечисление возможных результатов ожидания
		enum class WaitingStatus {
			SUCCESSFUL,			///< Ожидание завершено успешно
			INVALID_ID,			///< Некорректный ID задачи или суперзадачи
			TIMEOUT,			///< Ожидание завершилось с таймаутом
			SUPERTASK_CANCELED	///< Суперзадача завершена
		};

		/// \brief Структура WaitingResult
		/// \details Структура - описание информации об ожидания
		struct WaitingResult {
			WaitingStatus status;				///< Статус ожидания
			std::optional<TaskInfo> taskInfo;	///< Информация о задаче
		};

		/// Перечисление возможных состояний пула потоков
		enum class ThreadPoolState {
			NOT_WORKING,	///< Пул потоков не работает
			STARTING,		///< Пул потоков запускается
			WORKING,		///< Пул потоков работает
			STOPPING		///< Пул потоков останавливается
		};

		/// Конструктор
		ThreadPool() : _nThreads(0), _state(ThreadPoolState::NOT_WORKING), _newTaskId(0), _totalFinalizedTasks(0) { }

		/// Конструктор копирования запрещен
		ThreadPool(const ThreadPool&) = delete;

		/// Оператор копирующего присваивания запрещен
		ThreadPool& operator=(const ThreadPool&) = delete;

		/// Перемещающий конструктор запрещен
		ThreadPool(ThreadPool&&) = delete;

		/// Оператор перемещающего присваивания запрещен
		ThreadPool& operator=(ThreadPool&&) = delete;

		/// Старт пула потоков
		/// \return Успешность старта
		bool start(const size_t& nThreads) {
			// Устанавливаем количество потоков
			_nThreads = nThreads;

			// Резервируем место под потоки
			_threads.reserve(_nThreads);

			// Устанавливаем состояние пула потоков в "Пул потоков запускается"
			ThreadPoolState expectedState = ThreadPoolState::NOT_WORKING;
			bool started = _state.compare_exchange_strong(expectedState, ThreadPoolState::STARTING);
			if (!started) {
				return false;
			}

			// Инициализируем потоки
			for (size_t i = 0; i < _nThreads; ++i) {
				_threads.emplace_back(&ThreadPool::run, this);
			}

			// Устанавливаем состояние пула потоков в "Пул потоков работает"
			_state = ThreadPoolState::WORKING;
			return true;
		}

		/// Остановка пула потоков
		/// \return Успешность остановки
		bool stop() {
			// Устанавливаем состояние пула потоков в "Пул потоков останавливается"
			ThreadPoolState expectedState = ThreadPoolState::WORKING;
			bool stopped = _state.compare_exchange_strong(expectedState, ThreadPoolState::STOPPING);
			if (!stopped) {
				return false;
			}
			// Уведомляем все потоки об остановке
			_tasksCv.notify_all();

			// Отмена всех существующих задач, которые не были выполнены
			std::unique_lock<std::mutex> tasksInfoLock(_tasksInfoMtx);
			for (auto it = _tasksInfo.begin(); it != _tasksInfo.end(); ++it) {
				if (it->second.status != TaskStatus::COMPLETED) {
					it->second.status = TaskStatus::CANCELED;
					++_totalFinalizedTasks;
				}
			}
			tasksInfoLock.unlock();
			// Уведомляем все потоки об отмененных задачах
			_tasksInfoCv.notify_all();

			// Дожидаемся завершения работы всех потоков
			for (size_t i = 0; i < _threads.size(); ++i) {
				_threads[i].join();
			}

			// Очищаем контейнеры
			_tasksInfo.clear();
			while (_tasks.size()) {
				_tasks.pop();
			}
			_threads.clear();

			// Обнуляем количественные переменные
			_nThreads = 0;
			_newTaskId = 0;
			_totalFinalizedTasks = 0;

			// Устанавливаем состояние пула потоков в "Пул потоков не работает"
			_state = ThreadPoolState::NOT_WORKING;

			return true;
		}

		/// Деструктор - завершение работы пула потоков
		~ThreadPool() {
			stop();
		}

		/// Добавление задачи в очередь
		/// \param[in] function - функция, которую нужно выполнить
		/// \return ID добавленной задачи
		TaskId addTask(const std::function<std::any()>& function) {
			if (_state == ThreadPoolState::WORKING) {
				// Выдаем новый ID
				TaskId taskId = _newTaskId++;

				std::unique_lock<std::mutex> tasksInfoLock(_tasksInfoMtx);
				_tasksInfo[taskId].status = TaskStatus::CREATED;
				tasksInfoLock.unlock();

				// Добавляем новую задачу в очередь
				std::unique_lock<std::mutex> tasksLock(_tasksMtx);
				_tasks.emplace(taskId, function);
				tasksLock.unlock();

				_tasksInfoCv.notify_all();

				// Уведомляем один из потоков, что появилась новая задача в очереди
				_tasksCv.notify_one();

				return taskId;
			}
			else {
				return std::nullopt;
			}
		}

		/// Добавление задачи, которая ничего не возвращает в очередь
		/// \param[in] function - функция, которую нужно выполнить
		/// \return ID добавленной задачи
		TaskId addVoidTask(const std::function<void()>& function) {
			return addTask([function]() {
				function();
				return std::any();
			});
		}

		/// Отмена задачи
		/// \param[in] taskId - ID отменяемой задачи
		void cancelTask(const TaskId& taskId) {
			std::unique_lock<std::mutex> tasksInfoLock(_tasksInfoMtx);
			if (!valid(taskId) || finalized(taskId)) {
				return;
			}
			_tasksInfo[taskId].status = TaskStatus::CANCELED;
			++_totalFinalizedTasks;
			tasksInfoLock.unlock();
			_tasksInfoCv.notify_all();
		}

		/// Ожидание выполнения задачи
		/// \param[in] taskId - ID задачи, выполнение которой ожидается
		/// \return Статус ожидания
		WaitingStatus wait(const TaskId& taskId) {
			std::unique_lock<std::mutex> tasksInfoLock(_tasksInfoMtx);
			return wait(tasksInfoLock, taskId);
		}

		/// Ожидание выполнения задачи с проверкой отмены суперзадачи
		/// \param[in] taskId		- ID задачи, выполнение которой ожидается
		/// \param[in] superTaskId	- ID cуперзадачи, отмена которой проверяется
		/// \return Статус ожидания
		WaitingStatus wait(const TaskId& taskId, const TaskId& superTaskId) {
			std::unique_lock<std::mutex> tasksInfoLock(_tasksInfoMtx);
			return wait(tasksInfoLock, taskId, superTaskId);
		}

		/// Ожидание выполнения задачи с таймаутом
		/// \param[in] taskId	- ID задачи, выполнение которой ожидается
		/// \param[in] timeout	- таймаут в миллисекундах
		/// \return Статус ожидания
		WaitingStatus wait(const TaskId& taskId, const std::chrono::milliseconds& timeout) {
			std::unique_lock<std::mutex> tasksInfoLock(_tasksInfoMtx);
			return wait(tasksInfoLock, taskId, timeout);
		}

		/// Ожидание выполнения задачи с таймаутом с проверкой отмены суперзадачи
		/// \param[in] taskId		- ID задачи, выполнение которой ожидается
		/// \param[in] superTaskId	- ID cуперзадачи, отмена которой проверяется
		/// \param[in] timeout		- таймаут в миллисекундах
		/// \return Статус ожидания
		WaitingStatus wait(const TaskId& taskId, const TaskId& superTaskId, const std::chrono::milliseconds& timeout) {
			std::unique_lock<std::mutex> tasksInfoLock(_tasksInfoMtx);
			return wait(tasksInfoLock, taskId, superTaskId, timeout);
		}

		/// Ожидание выполнения задачи с получением результата выполнения
		/// \param[in] taskId - ID задачи, выполнение которой ожидается
		/// \return Результат ожидания
		WaitingResult waitResult(const TaskId& taskId) {
			std::unique_lock<std::mutex> tasksInfoLock(_tasksInfoMtx);
			WaitingResult result;
			result.status = wait(tasksInfoLock, taskId);
			if (result.status == WaitingStatus::SUCCESSFUL) {
				result.taskInfo = _tasksInfo[taskId];
			}
			return result;
		}

		/// Ожидание выполнения задачи с получением результата выполнения с проверкой отмены суперзадачи
		/// \param[in] taskId		- ID задачи, выполнение которой ожидается
		/// \param[in] superTaskId	- ID cуперзадачи, отмена которой проверяется
		/// \return Результат ожидания
		WaitingResult waitResult(const TaskId& taskId, const TaskId& superTaskId) {
			std::unique_lock<std::mutex> tasksInfoLock(_tasksInfoMtx);
			WaitingResult result;
			result.status = wait(tasksInfoLock, taskId, superTaskId);
			if (result.status == WaitingStatus::SUCCESSFUL) {
				result.taskInfo = _tasksInfo[taskId];
			}
			return result;
		}

		/// Ожидание выполнения задачи с получением результата выполнения с таймаутом
		/// \param[in] taskId	- ID задачи, выполнение которой ожидается
		/// \param[in] timeout	- таймаут в миллисекундах
		/// \return Результат ожидания
		WaitingResult waitResult(const TaskId& taskId, const std::chrono::milliseconds& timeout) {
			std::unique_lock<std::mutex> tasksInfoLock(_tasksInfoMtx);
			WaitingResult result;
			result.status = wait(tasksInfoLock, taskId, timeout);
			if (result.status == WaitingStatus::SUCCESSFUL) {
				result.taskInfo = _tasksInfo[taskId];
			}
			return result;
		}

		/// Ожидание выполнения задачи с получением результата выполнения с таймаутом с проверкой отмены суперзадачи
		/// \param[in] taskId		- ID задачи, выполнение которой ожидается
		/// \param[in] superTaskId	- ID cуперзадачи, отмена которой проверяется
		/// \param[in] timeout		- таймаут в миллисекундах
		/// \return Результат ожидания
		WaitingResult waitResult(const TaskId& taskId, const TaskId& superTaskId, const std::chrono::milliseconds& timeout) {
			std::unique_lock<std::mutex> tasksInfoLock(_tasksInfoMtx);
			WaitingResult result;
			result.status = wait(tasksInfoLock, taskId, superTaskId, timeout);
			if (result.status == WaitingStatus::SUCCESSFUL) {
				result.taskInfo = _tasksInfo[taskId];
			}
			return result;
		}

		/// Ожидание выполнения всех задач
		void waitAll() {
			std::unique_lock<std::mutex> tasksInfoLock(_tasksInfoMtx);
			_tasksInfoCv.wait(tasksInfoLock, [this]() {
				return _totalFinalizedTasks == _newTaskId;
			});
		}

		/// Ожидание выполнения всех задач
		/// \param[in] timeout - таймаут в миллисекундах
		/// \return Успешность ожидания
		bool waitAll(const std::chrono::milliseconds& timeout) {
			std::unique_lock<std::mutex> tasksInfoLock(_tasksInfoMtx);
			return _tasksInfoCv.wait_for(tasksInfoLock, timeout, [this]() {
				return _totalFinalizedTasks == _newTaskId;
			});
		}

		/// Удаление результата завершенной задачи
		/// \param[in] taskId - ID задачи, результат которой следует удалить
		void removeResult(const TaskId& taskId) {
			std::unique_lock<std::mutex> tasksInfoLock(_tasksInfoMtx);
			if (valid(taskId) && finalized(taskId)) {
				_tasksInfo[taskId].result.reset();
			}
		}

		/// Проверка статуса задачи (создана)
		/// \param[in] taskId - ID задачи, статус которой проверяется
		/// \return Результат проверки
		bool isCreated(const TaskId& taskId) {
			std::unique_lock<std::mutex> tasksInfoLock(_tasksInfoMtx);
			return valid(taskId) && created(taskId);
		}

		/// Проверка статуса задачи (выполняется)
		/// \param[in] taskId - ID задачи, статус которой проверяется
		/// \return Результат проверки
		bool isStarted(const TaskId& taskId) {
			std::unique_lock<std::mutex> tasksInfoLock(_tasksInfoMtx);
			return valid(taskId) && started(taskId);
		}

		/// Проверка статуса задачи (выполнена)
		/// \param[in] taskId - ID задачи, статус которой проверяется
		/// \return Результат проверки
		bool isCompleted(const TaskId& taskId) {
			std::unique_lock<std::mutex> tasksInfoLock(_tasksInfoMtx);
			return valid(taskId) && completed(taskId);
		}

		/// Проверка статуса задачи (отменена)
		/// \param[in] taskId - ID задачи, статус которой проверяется
		/// \return Результат проверки
		bool isCanceled(const TaskId& taskId) {
			std::unique_lock<std::mutex> tasksInfoLock(_tasksInfoMtx);
			return valid(taskId) && canceled(taskId);
		}

	private:
		/// Основная функция потока
		void run() {
			// Если пул не завршается
			while (_state != ThreadPoolState::STOPPING) {
				// Если нет задач, ожидаем уведомления о новой задаче или о завершении
				std::unique_lock<std::mutex> tasksLock(_tasksMtx);
				_tasksCv.wait(tasksLock, [this]()->bool { return !_tasks.empty() || _state == ThreadPoolState::STOPPING; });

				// Если поступила новая задача, то достаем ее из очереди
				std::optional<Task> optionalTask;
				if (!_tasks.empty() && _state == ThreadPoolState::WORKING) {
					optionalTask = std::move(_tasks.front());
					_tasks.pop();
				}

				tasksLock.unlock();

				// Если достали задачу из очереди
				if (optionalTask) {
					// Получаем ID задачи
					const Task& task = *optionalTask;
					const TaskId& taskId = task.id();

					bool isTaskCanceled = false;

					// Обновляем информацию о задаче
					std::unique_lock<std::mutex> tasksInfoLock(_tasksInfoMtx);
					if (canceled(taskId)) {
						isTaskCanceled = true;
					}
					else {
						_tasksInfo[taskId].status = TaskStatus::STARTED;
					}
					tasksInfoLock.unlock();

					// Уведомляем все потоки о том, что информация о задачах обновилась
					_tasksInfoCv.notify_all();

					// Если задача была отменена, то пропускаем
					if (isTaskCanceled) {
						continue;
					}

					// Выполняем задачу
					const std::any& result = task();
					// Обновляем информацию о задаче
					tasksInfoLock.lock();
					if (canceled(taskId)) {
						isTaskCanceled = true;
					}
					tasksInfoLock.unlock();
					// Если задача была отменена в процессе выполнения, то пропускаем
					if (isTaskCanceled) {
						continue;
					}
					// Обновляем информацию о задаче
					tasksInfoLock.lock();
					_tasksInfo[taskId].result = result;
					_tasksInfo[taskId].status = TaskStatus::COMPLETED;
					++_totalFinalizedTasks;
					tasksInfoLock.unlock();

					// Уведомляем все потоки о том, что информация о задачах обновилась
					_tasksInfoCv.notify_all();
				}
			}
		}

		/// \brief Класс Task
		/// \details Класс - описание задачи которая будет помещена в очередь задач, а затем выполнена одним из потоков
		class Task {
		public:
			/// Конструктор - создание задачи
			/// \param[in] id		- ID текущей задачи
			/// \param[in] function	- функция, которую нужно выполнить
			Task(const TaskId& id, const std::function<std::any()>& function) : _id(id), _function(function) {}

			/// Выполнение текущей задачи
			/// \return Результат выполнения текущей задачи
			std::any operator() () const {
				return _function();
			}

			/// Получение ID текущей задачи
			/// \return ID текущей задачи
			TaskId id() const {
				return _id;
			}

		private:
			TaskId _id;								///< ID задачи
			std::function<std::any()> _function;	///< Функция, которую нужно выполнить
		};

		/// Ожидание выполнения задачи с использованием существующей блокировки
		/// \param[in] tasksInfoLock	- блокировка
		/// \param[in] taskId			- ID задачи, выполнение которой ожидается
		/// \return Статус ожидания
		WaitingStatus wait(std::unique_lock<std::mutex>& tasksInfoLock, const TaskId& taskId) {
			if (!valid(taskId)) {
				return WaitingStatus::INVALID_ID;
			}
			_tasksInfoCv.wait(tasksInfoLock, [this, &taskId]() { return finalized(taskId); });
			return WaitingStatus::SUCCESSFUL;
		}

		/// Ожидание выполнения задачи с проверкой отмены суперзадачи с использованием существующей блокировки
		/// \param[in] tasksInfoLock	- блокировка
		/// \param[in] taskId			- ID задачи, выполнение которой ожидается
		/// \param[in] superTaskId		- ID cуперзадачи, отмена которой проверяется
		/// \return Статус ожидания
		WaitingStatus wait(std::unique_lock<std::mutex>& tasksInfoLock, const TaskId& taskId, const TaskId& superTaskId) {
			if (!valid(taskId) || !valid(superTaskId)) {
				return WaitingStatus::INVALID_ID;
			}
			_tasksInfoCv.wait(tasksInfoLock, [this, &taskId, &superTaskId]() { return finalized(taskId) || canceled(superTaskId); });
			if (canceled(superTaskId)) {
				return WaitingStatus::SUPERTASK_CANCELED;
			}
			return WaitingStatus::SUCCESSFUL;
		}

		/// Ожидание выполнения задачи с таймаутом с использованием существующей блокировки
		/// \param[in] tasksInfoLock	- блокировка
		/// \param[in] taskId			- ID задачи, выполнение которой ожидается
		/// \param[in] timeout			- таймаут в миллисекундах
		/// \return Статус ожидания
		WaitingStatus wait(std::unique_lock<std::mutex>& tasksInfoLock, const TaskId& taskId, const std::chrono::milliseconds& timeout) {
			if (!valid(taskId)) {
				return WaitingStatus::INVALID_ID;
			}
			bool success = _tasksInfoCv.wait_for(tasksInfoLock, timeout, [this, &taskId]() { return finalized(taskId); });
			if (!success) {
				return WaitingStatus::TIMEOUT;
			}
			return WaitingStatus::SUCCESSFUL;
		}

		/// Ожидание выполнения задачи с таймаутом с проверкой отмены суперзадачи с использованием существующей блокировки
		/// \param[in] tasksInfoLock	- блокировка
		/// \param[in] taskId			- ID задачи, выполнение которой ожидается
		/// \param[in] superTaskId		- ID cуперзадачи, отмена которой проверяется
		/// \param[in] timeout			- таймаут в миллисекундах
		/// \return Успешность ожидания
		WaitingStatus wait(std::unique_lock<std::mutex>& tasksInfoLock, const TaskId& taskId, const TaskId& superTaskId, const std::chrono::milliseconds& timeout) {
			if (!valid(taskId) || !valid(superTaskId)) {
				return WaitingStatus::INVALID_ID;
			}
			bool success = _tasksInfoCv.wait_for(tasksInfoLock, timeout, [this, &taskId, &superTaskId]() { return finalized(taskId) || canceled(superTaskId); });
			if (!success) {
				return WaitingStatus::TIMEOUT;
			}
			if (canceled(superTaskId)) {
				return WaitingStatus::SUPERTASK_CANCELED;
			}
			return WaitingStatus::SUCCESSFUL;
		}

		/// Неблокирующая проверка валидности ID задачи
		/// \param[in] taskId - ID задачи, валидность которого проверяется
		/// \return Результат проверки
		bool valid(const TaskId& taskId) const {
			if (!taskId) {
				return false;
			}
			return _tasksInfo.contains(taskId);
		}

		/// Неблокирующее получение статуса задачи
		/// \param[in] taskId - ID задачи, статус которой получается
		/// \return Статус задачи
		TaskStatus status(const TaskId& taskId) const {
			return _tasksInfo.at(taskId).status;
		}

		/// Неблокирующая проверка статуса задачи (создана)
		/// \param[in] taskId - ID задачи, статус которой проверяется
		/// \return Результат проверки
		bool created(const TaskId& taskId) const {
			return status(taskId) == TaskStatus::CREATED;
		}

		/// Неблокирующая проверка статуса задачи (выполняется)
		/// \param[in] taskId - ID задачи, статус которой проверяется
		/// \return Результат проверки
		bool started(const TaskId& taskId) const {
			return status(taskId) == TaskStatus::STARTED;
		}

		/// Неблокирующая проверка статуса задачи (выполнена)
		/// \param[in] taskId - ID задачи, статус которой проверяется
		/// \return Результат проверки
		bool completed(const TaskId& taskId) const {
			return status(taskId) == TaskStatus::COMPLETED;
		}

		/// Неблокирующая проверка статуса задачи (отменена)
		/// \param[in] taskId - ID задачи, статус которой проверяется
		/// \return Результат проверки
		bool canceled(const TaskId& taskId) const {
			return status(taskId) == TaskStatus::CANCELED;
		}

		/// Неблокирующая проверка статуса задачи (завершена)
		/// \param[in] taskId - ID задачи, статус которой проверяется
		/// \return Результат проверки
		bool finalized(const TaskId& taskId) const {
			return completed(taskId) || canceled(taskId);
		}

		size_t _nThreads;					///< Количество потоков
		std::vector<std::thread> _threads;	///< Потоки

		std::queue<Task> _tasks;			///< Очередь задач
		std::condition_variable _tasksCv;	///< Переменная синхронизации работы с очередью задач
		std::mutex _tasksMtx;				///< Мьютекс для синхронизации работы с очередью задач

		std::unordered_map<TaskId, TaskInfo> _tasksInfo;	///< Информация о задачах
		std::condition_variable _tasksInfoCv;				///< Переменная синхронизации работы с информацией о задачах
		std::mutex _tasksInfoMtx;							///< Мьютекс для синхронизации работы с информацией о задачах

		std::atomic<ThreadPoolState> _state;		///< Состояние пула потоков
		std::atomic<size_t> _newTaskId;				///< ID следующей задачи
		std::atomic<size_t> _totalFinalizedTasks;	///< Количество завершенных (выполненных + отмененных) задач
	};

}

#endif // !THREAD_POOL_H
