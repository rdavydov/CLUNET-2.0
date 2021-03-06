/**************************************************************************************

The MIT License (MIT)

Copyright (c) 2016 Sergey V. DUDANOV

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

*****************************************************************************************/

#include "defines.h"
#include "clunet_config.h"
#include "bits.h"
#include "clunet.h"

#include <stdint.h>
#include <avr/io.h>
#include <avr/interrupt.h>


/* Указатели на функции обратного вызова при получении пакетов (должны быть как можно короче) (ОЗУ: 4 байта при МК с 16-битной адресацией) */
static void (*cbDataReceived)(uint8_t src_address, uint8_t command, char* data, uint8_t size) = 0;
static void (*cbDataReceivedSniff)(uint8_t src_address, uint8_t dst_address, uint8_t command, char* data, uint8_t size) = 0;

/* Глобальные статические переменные (ОЗУ: 5 байт) */
static uint8_t sendingState = CLUNET_SENDING_IDLE; // Состояние передачи
static uint8_t sendingLength; // Длина данных для отправки вместе с заголовком кадра
static uint8_t sendingPriority; // Приоритет отправляемого пакета (от 1 до 8)
static uint8_t readingState = CLUNET_READING_IDLE; // Состояние чтения
static uint8_t readingActiveBits; // Количество активных прочитанных бит

/* Буферы данных */
static char sendBuffer[CLUNET_SEND_BUFFER_SIZE]; // Буфер передачи
static char readBuffer[CLUNET_READ_BUFFER_SIZE]; // Буфер чтения

#ifdef CLUNET_DEVICE_NAME
const static char devName[] = CLUNET_DEVICE_NAME; // Имя устройства если задано (простое лаконичное)
#endif

/* Функция нахождения контрольной суммы Maxim iButton 8-bit */
static char
check_crc(const char* data, const uint8_t size)
{
      char crc = 0;
      uint8_t a = 0;
      do
      {
            uint8_t b = 8;
            char inbyte = data[a];
            do
            {
                  uint8_t mix = (crc ^ inbyte) & 1;
                  crc >>= 1;
                  if (mix) crc ^= 0x8C;
                  inbyte >>= 1;
            }
            while (--b);
      }
      while (++a < size);
      return crc;
}

/* Встраиваемая функция обработки входящего пакета */
static inline void
clunet_data_received(const uint8_t src_address, const uint8_t dst_address, const uint8_t command, char* data, const uint8_t size)
{
	if (cbDataReceivedSniff)
		(*cbDataReceivedSniff)(src_address, dst_address, command, data, size);

	if ((src_address != CLUNET_DEVICE_ID) && ((dst_address == CLUNET_DEVICE_ID) || (dst_address == CLUNET_BROADCAST_ADDRESS)))
	{
		/* Команда перезагрузки */
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
				clunet_send(src_address, CLUNET_PRIORITY_MESSAGE, CLUNET_COMMAND_DISCOVERY_RESPONSE, devName, sizeof(devName) - 1);
				#else
				clunet_send(src_address, CLUNET_PRIORITY_MESSAGE, CLUNET_COMMAND_DISCOVERY_RESPONSE, 0, 0);
				#endif
				break;

			/* Ответ на пинг */
			case CLUNET_COMMAND_PING:

				clunet_send(src_address, CLUNET_PRIORITY_COMMAND, CLUNET_COMMAND_PING_REPLY, data, size);
			}
			return;
		}
		if (cbDataReceived)
			(*cbDataReceived)(src_address, command, data, size);
	}
}

/* Процедура прерывания сравнения таймера (ОЗУ: 4 байта) */
ISR(CLUNET_TIMER_COMP_VECTOR)
{
	static uint8_t bitIndex, byteIndex, bitStuff, lastActiveBits; // Статические переменные в ОЗУ (4 байт)
	uint8_t numBits, prio;

	// Многоцелевая переменная-маска состояния линии и чтения бит данных
	const uint8_t lineFree = CLUNET_SENDING ? 0x80 : 0x00;

	/* Если передатчик освободился, сбросим статус чтения и пока сюда не планируем возвращаться */
	if (sendingState == CLUNET_SENDING_IDLE)
	{
		readingState = CLUNET_READING_IDLE;
		goto _disable_oci;
	}

	/* А если мы в ожидании освобождения линии, то начнем передачу через 1Т, предварительно сбросив статус чтения */
	else if (sendingState == CLUNET_SENDING_WAIT)
	{
		readingState = CLUNET_READING_IDLE;
		sendingState = CLUNET_SENDING_INIT;
		byteIndex = bitIndex = 0;
		bitStuff = 1;
_delay_1t:
		CLUNET_TIMER_REG_OCR += CLUNET_T;
		return;
	}

	// Если мы будем прижимать линию, то проверим совпадение переданных и полученных бит, если различны, то конфликт на линии - останавливаем передачу и ждем
	else if (!lineFree && readingActiveBits != lastActiveBits && !(sendingState == CLUNET_SENDING_INIT && !bitIndex))
	{
		sendingState = CLUNET_SENDING_WAIT;
		goto _disable_oci;
	}

	// Количество бит для передачи
	numBits = bitStuff;
	
	// Инверсия выхода
	CLUNET_SEND_INVERT;

	// Смотрим фазу передачи
	switch (sendingState)
	{
		// Главная фаза передачи данных
		case CLUNET_SENDING_DATA:
_send_data:
			// Если мы прижали линию, то ищем единичные биты, иначе - нулевые
			while (((sendBuffer[byteIndex] << bitIndex) & 0x80) ^ lineFree)
			{
				numBits++;
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
				/* Нам нужно не более 5 бит, выходим из цикла */
				if (numBits == 5)
					break;
			}
			
			break;

		// Фаза отправки заголовка кадра
		case CLUNET_SENDING_INIT:

			prio = (sendingPriority - 1) << 5;

			while (((prio << bitIndex) & 0x80) ^ lineFree)
			{
				numBits++;
				/* Если отправлены все биты приоритета */
				if(++bitIndex == 3)
				{
					bitIndex = 0;
					sendingState = CLUNET_SENDING_DATA;
					goto _send_data; // Уходим в часть кода, занимающейся отправкой данных
				}
			}

			break;

		// Фаза завершения передачи кадра и генерации стопового бита при необходимости
		case CLUNET_SENDING_STOP:

			// Если линию отпустили, то стоповый бит не требуется, во внешнем прерывании запланируются все необходимые действия
			if (lineFree)
			{
				sendingState = CLUNET_SENDING_IDLE;
_disable_oci:
				CLUNET_DISABLE_TIMER_COMP;
				return;
			}

			// Иначе если заняли, то сделаем короткий стоповый импульс длительностью 1Т
			else
				goto _delay_1t;

	}

	// Сохраним сколько единичных бит мы должны передать (на сколько периодов прижать линию)
	if (!lineFree)
		lastActiveBits = numBits;

	CLUNET_TIMER_REG_OCR += CLUNET_T * numBits;

	bitStuff = (numBits == 5);

}
/* Конец ISR(CLUNET_TIMER_COMP_VECTOR) */


/* Процедура внешнего прерывания по фронту и спаду сигнала (ОЗУ: 5 байта) */
ISR(CLUNET_INT_VECTOR)
{

	static uint8_t bitIndex, byteIndex, bitStuff, tickSync, bitNumSync; // Статические переменные (ОЗУ: 5 байт)

	// Текущее значение таймера
	const uint8_t now = CLUNET_TIMER_REG;

	// Многоцелевая переменная состояния линии и заполнения байт соответствующими значениями
	const uint8_t lineFree = CLUNET_READING ? 0x00 : 0xFF;

	// Количество прочитанных бит
	uint8_t bitNum = 0;

	if (readingState)
	{

		uint8_t ticks, period;

		// Цикл подсчета количества бит с момента последней синхронизации по спаду
		for (ticks = now - tickSync, period = (CLUNET_T / 2); ticks >= period; period += CLUNET_T)
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

	// Если линию освободило
	if (lineFree)
	{
		// Сохраняем количество прочитанных бит после синхронизации (используется в чтении)
		bitNumSync = bitNum;
		
		// Если состояние передачи неактивно либо в ожидании, то запланируем сброс чтения и, при необходимости, начало передачи через 7Т
		if (!(sendingState & 3))
		{
			CLUNET_TIMER_REG_OCR = now + (7*CLUNET_T);
			CLUNET_ENABLE_TIMER_COMP;
		}
		
		// Если состояние передачи активно
		else
			// Сохраним количество прочитанных активных бит для проверки в процедуре передачи
			readingActiveBits = bitNum;
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
			// Разница должна быть в идеале -1, так как прерывание сравнения вызывается на OCR+1, но так как нам ясно, что прерывания не было,
			// то при 0, а тем более -1 мы в него попадем после выхода отсюда, поэтому эту разницу мы должны проигнорировать
			// Да, и разница < 0, естественно, из области фантастики.
			int8_t delta = CLUNET_TIMER_REG_OCR - now;

			const int8_t max_delta = (int8_t)((float)CLUNET_T * 0.3f);

			// Если разница во времени когда мы должны прижать линию более 0.3Т, то конфликт однозначен - отдаем линию другому устройству (арбитраж проигран), но чтение продолжаем :)
			if (delta >= max_delta)
			{
				CLUNET_DISABLE_TIMER_COMP;
				sendingState = CLUNET_SENDING_WAIT;
			}
			// Если разница во времени менее 0.3Т, то это рассинхронизация, синхронизируем регистр сравнения и быстро попадаем в прерывание сравнения для продолжения передачи
			else if (delta > 0)
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
			bitIndex += bitNum - bitStuff;

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
/* Конец ISR(CLUNET_INT_VECTOR) */

void
clunet_init()
{
	sei();
	CLUNET_TIMER_INIT;
	CLUNET_PIN_INIT;
	CLUNET_INT_INIT;
	char reset_source = MCUSR;
	clunet_send (
		CLUNET_BROADCAST_ADDRESS,
		CLUNET_PRIORITY_MESSAGE,
		CLUNET_COMMAND_BOOT_COMPLETED,
		&reset_source,
		sizeof(reset_source)
	);
	MCUSR = 0;
}

void
clunet_send(const uint8_t address, const uint8_t prio, const uint8_t command, const char* data, const uint8_t size)
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
			while (++idx < size);
		}

		/* Добавляем контрольную сумму */
		sendBuffer[CLUNET_OFFSET_DATA + size] = check_crc(sendBuffer, CLUNET_OFFSET_DATA + size);
		
		sendingLength = size + (CLUNET_OFFSET_DATA + 1);

		sendingState = CLUNET_SENDING_WAIT;	// Фаза ожидания линии

		// Если линия свободна, то запланируем отправку
		if (!CLUNET_READING)
		{
			CLUNET_TIMER_REG_OCR = CLUNET_TIMER_REG + (7*CLUNET_T);
			CLUNET_ENABLE_TIMER_COMP;
		}

		// Все равно отпустим линию (вдруг задержки на заряд паразитной емкости линии) и процедура внешнего прерывания сама запланирует отправку
		CLUNET_SEND_0;

	}
}
/* Конец void clunet_send(.....) */

/* Возвращает 0, если готов к передаче, иначе приоритет текущей задачи */
uint8_t
clunet_ready_to_send()
{
	return sendingState ? sendingPriority : 0;
}

void
clunet_set_on_data_received(void (*f)(uint8_t src_address, uint8_t command, char* data, uint8_t size))
{
	cbDataReceived = f;
}

void
clunet_set_on_data_received_sniff(void (*f)(uint8_t src_address, uint8_t dst_address, uint8_t command, char* data, uint8_t size))
{
	cbDataReceivedSniff = f;
}
