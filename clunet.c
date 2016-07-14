#include "defines.h"
#include "clunet_config.h"
#include "bits.h"
#include "clunet.h"

#include <stdint.h>
#include <avr/io.h>
#include <avr/interrupt.h>


/* Функции обратного вызова при получении пакетов (должны быть как можно короче, так как вызываются прерываниями */
static void (*cbDataReceived)(uint8_t src_address, uint8_t command, uint8_t* data, uint8_t size) = 0;
static void (*cbDataReceivedSniff)(uint8_t src_address, uint8_t dst_address, uint8_t command, uint8_t* data, uint8_t size) = 0;

static uint8_t sendingState = CLUNET_SENDING_IDLE;
static uint8_t readingState = CLUNET_READING_IDLE;
static uint8_t sendingLength;
static uint8_t sendingPriority;

static uint8_t sendBuffer[CLUNET_SEND_BUFFER_SIZE];
static uint8_t readBuffer[CLUNET_READ_BUFFER_SIZE];

#ifdef CLUNET_DEVICE_NAME
static uint8_t devName[] = CLUNET_DEVICE_NAME;
#endif

static char
check_crc(const uint8_t* data, const uint8_t size)
{
      uint8_t crc = 0;
      uint8_t i, j;
      for (i = 0; i < size; i++)
      {
            uint8_t inbyte = data[i];
            for (j = 0 ; j < 8 ; j++)
            {
                  uint8_t mix = (crc ^ inbyte) & 1;
                  crc >>= 1;
                  if (mix) crc ^= 0x8C;
                  inbyte >>= 1;
            }
      }
      return crc;
}

static inline void
clunet_data_received(const uint8_t src_address, const uint8_t dst_address, const uint8_t command, uint8_t* data, const uint8_t size)
{
	if (cbDataReceivedSniff)
		(*cbDataReceivedSniff)(src_address, dst_address, command, data, size);

	if ((src_address != CLUNET_DEVICE_ID) && ((dst_address == CLUNET_DEVICE_ID) || (dst_address == CLUNET_BROADCAST_ADDRESS)))
	{
		// Команда перезагрузки
		if (command == CLUNET_COMMAND_REBOOT)
		{
			cli();
			set_bit(WDTCR, WDE);
			while(1);
		}

		if ((sendingState == CLUNET_SENDING_IDLE) || (sendingPriority <= CLUNET_PRIORITY_MESSAGE))
		{
			switch (command)
			{
			/* Ответ на поиск устройств */
			case CLUNET_COMMAND_DISCOVERY:

				#ifdef CLUNET_DEVICE_NAME
				clunet_send(src_address, CLUNET_PRIORITY_MESSAGE, CLUNET_COMMAND_DISCOVERY_RESPONSE, devName, sizeof(devName));
				#else
				clunet_send(src_address, CLUNET_PRIORITY_MESSAGE, CLUNET_COMMAND_DISCOVERY_RESPONSE, 0, 0);
				#endif
				break;

			/* Ответ на пинг */
			case CLUNET_COMMAND_PING:

				clunet_send(src_address, CLUNET_PRIORITY_COMMAND, CLUNET_COMMAND_PING_REPLY, data, size);
				break;
				
			default:

				if (cbDataReceived)
					(*cbDataReceived)(src_address, command, data, size);
				
			}
		}
	}
}

/* Процедура прерывания сравнения таймера */
ISR(CLUNET_TIMER_COMP_VECTOR)
{

	static uint8_t bitIndex, byteIndex, bitStuff;
	uint8_t numBits, lineFree, prio;

	// Если передатчик освободился, сбросим статус чтения и пока сюда не планируем возвращаться
	if (sendingState == CLUNET_SENDING_IDLE)
	{
		readingState = CLUNET_READING_IDLE;
		goto _send_end;
	}

	/* А если мы в ожидании освобождения линии, то начнем передачу через 1Т, предварительно сбросив статус чтения */
	else if (sendingState == CLUNET_SENDING_WAIT)
	{
		readingState = CLUNET_READING_IDLE;
		sendingState = CLUNET_SENDING_INIT;
		byteIndex = bitIndex = 0;
		bitStuff = 1;

_send_delay:

		CLUNET_TIMER_REG_OCR += CLUNET_T;
		return;
	}

	// Переменная количества передаваемых бит
	numBits = bitStuff;
	
	// Многоцелевая переменная-маска состояния линии и чтения бит данных
	lineFree = CLUNET_SENDING ? 0x80 : 0x00;

	CLUNET_SEND_INVERT;	// Инверсия выхода

	// Смотрим фазу передачи
	switch (sendingState)
	{
		// Главная фаза передачи данных
		case CLUNET_SENDING_DATA:
			
_send_data:
			// Если мы прижали линию, то ищем единичные биты, иначе - нулевые
			while (((sendBuffer[byteIndex] << bitIndex) & 0x80) ^ lineFree)
			{
				/* Если передан байт данных */
				if (++bitIndex & 8)
				{
					/* Если не все данные отосланы */
					if (++byteIndex < sendingLength)
						bitIndex = 0;		// начинаем передачу следующего байта с бита 0

					/* Иначе передача всех данных закончена */
					else
						sendingState = CLUNET_SENDING_STOP;
				}
				/* Если будем отправлять 5 одноименных бит, то запланируем битстаффинг в следующей передаче */
				if (++numBits == 5)
					break;
			}
			
			break;

		// Фаза отправки стартового бита, а также битов приоритета, старшего бита данных при условии их равенства единице
		case CLUNET_SENDING_INIT:

			prio = ((sendingPriority - 1) << 5);

			while (((prio << bitIndex) & 0x80) ^ lineFree)
			{
				numBits++;

				// Если отправлены все биты приоритета
				if(++bitIndex & 4)
				{
					sendingState = CLUNET_SENDING_DATA;
					bitIndex = 0;
					goto _send_data;
				}
			}

			break;

		case CLUNET_SENDING_STOP:

			// Если линию отпустили, то передача закончена, во внешнем прерывании запланируются необходимые действия
			if (lineFree)
			{
				sendingState = CLUNET_SENDING_IDLE;
_send_end:
				CLUNET_DISABLE_TIMER_COMP;
				return;
			}
			// Иначе если была отпущена (последний бит 0), то сделаем короткий импульс 1Т
			else
				goto _send_delay;

	}
	
	CLUNET_TIMER_REG_OCR += CLUNET_T * numBits;

	bitStuff = (numBits == 5);

}
/* Конец ISR(CLUNET_TIMER_COMP_VECTOR) */


/* Процедура внешнего прерывания по фронту и спаду сигнала */
ISR(CLUNET_INT_VECTOR)
{

	static uint8_t bitIndex, byteIndex, bitStuff, tickSync, bitNumSync;

	uint8_t now, lineFree, bitNum;
	
	
	now = CLUNET_TIMER_REG;		// Текущее значение таймера
	
	lineFree = CLUNET_READING ? 0x00 : 0xFF;	// Линия освобождена

	bitNum = 0;

	if (readingState)
	{

		uint8_t ticks, period;

		// Цикл подсчета количества бит с момента последней синхронизации по спаду
		for (ticks = tickSync - now, period = CLUNET_READ1, bitNum = 1 ; ticks >= period ; period += CLUNET_T)
		{
			if(++bitNum > 10)
			{
				readingState = CLUNET_READING_ERROR;
				break;
			}
		}

		bitNum -= bitNumSync;
		
		/* Ошибка: нет битстаффинга */
		if (bitNum >= 6)
			readingState = CLUNET_READING_ERROR;

	}

	// Если линию освободило (пришли единицы)
	if (lineFree)
	{

		// Если состояние передачи неактивно либо в ожидании, то запланируем сброс чтения и при необходимости начало передачи
		if (!(sendingState & 3))
		{
			CLUNET_TIMER_REG_OCR = CLUNET_TIMER_REG + 7*CLUNET_T;
			CLUNET_ENABLE_TIMER_COMP;
		}

		bitNumSync = bitNum;	// Сохраняем количество прочитанных бит после синхронизации

	}
	// Если линию прижало к нулю (все устройства синхронизируются по спаду в независимости от состояний чтения и передачи)
	else
	{

		/* СИНХРОНИЗАЦИЯ ЧТЕНИЯ */
		tickSync = now;		// Синхронизация времени чтения
		bitNumSync = 0;		// Сброс прочитанных бит после синхронизации

		/* СИНХРОНИЗАЦИЯ ПЕРЕДАЧИ И АРБИТРАЖ */
		// Проверка на конфликт передачи. Если мы в активной фазе передачи, и не жмем линию
		if ((sendingState & 3) && !CLUNET_SENDING)
		{
			// Если разница во времени когда мы должны прижать линию более 0.3Т, то конфликт однозначен - отдаем линию другому устройству (арбитраж проигран), но чтение продолжаем :)
			if((CLUNET_TIMER_REG_OCR - now) > (uint8_t)((float)CLUNET_T * 0.3f))
			{
				CLUNET_DISABLE_TIMER_COMP;
				sendingState = CLUNET_SENDING_WAIT;
			}

			// Если разница во времени менее 0.3Т, то это рассинхронизация, синхронизируем регистр сравнения и быстро попадаем в прерывание сравнения для продолжения передачи
			else
				CLUNET_TIMER_REG_OCR = now;
		}

		// Если в ожидании приема пакета, то переходим к фазе начала приемки кадра, обнуляем счетчики и выходим
		if (!readingState)
		{
			readingState = CLUNET_READING_START;
			bitStuff = byteIndex = bitIndex = 0;
			return;
		}
	}

	switch (readingState)
	{
		// Главная фаза чтения данных
		case CLUNET_READING_DATA:

			// Если линия освободилась, значит была единичная посылка - установим соответствующие биты
			if (lineFree)
				readBuffer[byteIndex] |= (255 >> bitIndex);

			// Если линия прижалась, значит была нулевая посылка - сбросим соответствующие биты
			else
				readBuffer[byteIndex] &= ~(255 >> bitIndex);

			// Обновим битовый индекс с учетом битстаффинга
			bitIndex += (bitNum - bitStuff);

			if (bitIndex & 8)
			{
				// Если пакет прочитан полностью, то проверим контрольную сумму
				if ((++byteIndex > CLUNET_OFFSET_SIZE) && (byteIndex > readBuffer[CLUNET_OFFSET_SIZE] + CLUNET_OFFSET_DATA))
				{
					readingState = CLUNET_READING_IDLE;
					// Проверяем CRC, при успехе начнем обработку принятого пакета
					if (!check_crc(readBuffer, byteIndex))
						clunet_data_received (
							readBuffer[CLUNET_OFFSET_SRC_ADDRESS],
							readBuffer[CLUNET_OFFSET_DST_ADDRESS],
							readBuffer[CLUNET_OFFSET_COMMAND],
							readBuffer + CLUNET_OFFSET_DATA,
							readBuffer[CLUNET_OFFSET_SIZE]
						);

				}

				// Если данные прочитаны не полностью и мы не выходим за пределы буфера, то присвоим очередной байт и подготовим битовый индекс
				else if (byteIndex < CLUNET_READ_BUFFER_SIZE)
				{
					bitIndex &= 7;
					readBuffer[byteIndex] = lineFree;
				}

				// Иначе ошибка: нехватка приемного буфера -> игнорируем пакет
				else
					readingState = CLUNET_READING_ERROR;

			}

			break;

		// Фаза приемки начала кадра
		case CLUNET_READING_START:
			
			bitIndex += bitNum;

			// Если прочитаны биты начала кадра (1-ый стартовый, 2-4 приоритет)
			if (bitIndex >= 4)
			{
				readBuffer[0] = lineFree;
				bitIndex -= 4;							// Коррекция индекса чтения бита
				readingState = CLUNET_READING_DATA;		// К следующей фазе чтения данных
			}

	}

	/* Проверка на битстаффинг, учитываем в следующем цикле */
	bitStuff = (bitNum == 5);

}

void
clunet_init()
{
	sei();
	CLUNET_SEND_INIT;
	CLUNET_READ_INIT;
	CLUNET_TIMER_INIT;
	CLUNET_INIT_INT;
	uint8_t reset_source = MCUCSR;
	clunet_send (
		CLUNET_BROADCAST_ADDRESS,
		CLUNET_PRIORITY_MESSAGE,
		CLUNET_COMMAND_BOOT_COMPLETED,
		&reset_source,
		sizeof(reset_source)
	);
	MCUCSR = 0;
}

void
clunet_send(const uint8_t address, const uint8_t prio, const uint8_t command, const uint8_t* data, const uint8_t size)
{
	/* Если размер данных в пределах буфера передачи (максимально для протокола 250 байт) */
	if (size < (CLUNET_SEND_BUFFER_SIZE - CLUNET_OFFSET_DATA))
	{

		CLUNET_DISABLE_TIMER_COMP;	// Запретим прерывание сравнения, тем самым запретим и отправку и исключим конкурентный доступ к буферу

		/* Заполняем переменные */
		sendingPriority = (prio > 8) ? 8 : prio ? : 1;
		sendBuffer[CLUNET_OFFSET_SRC_ADDRESS] = CLUNET_DEVICE_ID;
		sendBuffer[CLUNET_OFFSET_DST_ADDRESS] = address;
		sendBuffer[CLUNET_OFFSET_COMMAND] = command;
		sendBuffer[CLUNET_OFFSET_SIZE] = size;
		
		/* Есть данные для отправки? Тогда скопируем их в буфер */
		if (data && size)
		{
			uint8_t idx = 0;
			do
				sendBuffer[CLUNET_OFFSET_DATA + idx] = data[idx];
			while(++idx < size);
		}

		/* Добавляем контрольную сумму */
		sendBuffer[CLUNET_OFFSET_DATA + size] = check_crc(sendBuffer, CLUNET_OFFSET_DATA + size);
		
		sendingLength = size + (CLUNET_OFFSET_DATA + 1);

		sendingState = CLUNET_SENDING_WAIT;	// Фаза ожидания линии

		// Если линия свободна, то запланируем отправку
		if (!CLUNET_READING)
		{
			CLUNET_TIMER_REG_OCR = CLUNET_TIMER_REG + 7*CLUNET_T;
			CLUNET_ENABLE_TIMER_COMP;
		}

		// Все равно отпустим линию (вдруг задержки на заряд паразитной емкости линии) и процедура внешнего прерывания сама запланирует отправку
		CLUNET_SEND_0;

	}
}

/* Возвращает 0, если готов к передаче, иначе приоритет текущей задачи */
uint8_t
clunet_ready_to_send()
{
	return sendingState ? sendingPriority : 0;
}

void
clunet_set_on_data_received(void (*f)(uint8_t src_address, uint8_t command, uint8_t* data, uint8_t size))
{
	cbDataReceived = f;
}

void
clunet_set_on_data_received_sniff(void (*f)(uint8_t src_address, uint8_t dst_address, uint8_t command, uint8_t* data, uint8_t size))
{
	cbDataReceivedSniff = f;
}
