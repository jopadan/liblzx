#pragma once

#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <cstdbool>
#include <cstddef>
#include <ctime>
#include <cstring>
#include <array>
#include <vector>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <getopt.h>
#include <utime.h>

/* STRUCTURE Info_Header
{
  UBYTE ID[3]; 0 - "LZX"
  UBYTE flags; 3 - INFO_FLAG_#?
  UBYTE[6]; 4
} */ /* SIZE = 10 */

/* STRUCTURE Archive_Header
{
  UBYTE attributes; 0 - HDR_PROT_#?
  UBYTE; 1
  ULONG unpacked_length; 2 - FUCKED UP LITTLE ENDIAN SHIT
  ULONG packed_length; 6 - FUCKED UP LITTLE ENDIAN SHIT
  UBYTE machine_type; 10 - HDR_TYPE_#?
  UBYTE pack_mode; 11 - HDR_PACK_#?
  UBYTE flags; 12 - HDR_FLAG_#?
  UBYTE; 13
  UBYTE len_comment; 14 - comment length [0,79]
  UBYTE extract_ver; 15 - version needed to extract
  UBYTE; 16
  UBYTE; 17
  ULONG date; 18 - Packed_Date
  ULONG data_crc; 22 - FUCKED UP LITTLE ENDIAN SHIT
  ULONG header_crc; 26 - FUCKED UP LITTLE ENDIAN SHIT
  UBYTE filename_len; 30 - filename length
} */ /* SIZE = 31 */

/* STRUCTURE DATE_Unpacked
{
  UBYTE year; 80 - Year 0=1970 1=1971 63=2033
  UBYTE month; 81 - 0=january 1=february .. 11=december
  UBYTE day; 82
  UBYTE hour; 83
  UBYTE minute; 84
  UBYTE second; 85
} */ /* SIZE = 6 */

/* STRUCTURE DATE_Packed
{
  UBYTE packed[4]; bit 0 is MSB, 31 is LSB
; bit # 0-4=Day 5-8=Month 9-14=Year 15-19=Hour 20-25=Minute 26-31=Second
} */ /* SIZE = 4 */
namespace lzx
{
	enum packer
	{
		original   = 0, // unpatched original LZX by Jonathan Forbes
		titov      = 1, // LZX 1.21 with patch by Andrey Titov aka dr.Titus
		calusinski = 2, // LZX 1.21 with patch by Mikolaj Calusinski
	};
	enum packer packer = packer::original;

	namespace time
	{
		using packed = std::array<uint8_t, 4>;
		namespace shift
		{
		enum shift
		{
			day        = 27,
			month      = 23,
			year       = 17,
			hour       = 12,
			minute     = 6,
			second     = 0,
		};
		};
		namespace mask
		{
			enum mask
			{
				day        = 0xF8000000,
				month      = 0x07800000,
				year       = 0x007E0000,
				hour       = 0x0001F000,
				minute     = 0x00000FC0,
				second     = 0x0000003F,
			};
		};
		uint8_t day_of_week(uint16_t y, uint8_t m, uint8_t d)
		{
			m = m % 4800 - 3 + 4800 * 2;
			y = y % 400 + 400 + m / 12;
			m %= 12;
			d = d % 7 + 7;
			return (uint8_t)(((y + y / 4 - y / 100 + y / 400) + (m * 13 + 12) / 5 + d) % 7);
		}
		uint8_t day_of_year(uint16_t y, uint8_t m, uint8_t d)
		{
			int n1 = floor(275 * m / 9);
			int n2 = floor((m + 9) / 12);
			int n3 = (1 + floor((y  - 4 * floor(y / 4) + 2) / 3));
			return (uint8_t)(n1 - (n2 * n3) + d - 30);
		}

		uint16_t year_convert(uint16_t y, enum packer t = packer::titov)
		{
			switch(t)
			{
				case packer::titov:
					y += y < 8 ? 134 : (y < 30 ? 70 : (y < 58 ? 76 : 42));
					break;
				case packer::calusinski:
					if(y == 8 || y == 9)
						y = 80;
					y += y < 8 ? 134 : 70;
					break;
				default:
					y += y < 8 ? 106 : (y < 58 ? 70 : 42);
					break;
			}
			return y;
		}

		time_t to_stdc(uint8_t header[4])
		{
			uint32_t tmp = be32toh(*(uint32_t*)header);
			struct tm tm = {
				static_cast<int>((tmp & mask::second) >> shift::second),
				static_cast<int>((tmp & mask::minute) >> shift::minute),
				static_cast<int>((tmp & mask::hour)   >> shift::hour),
				static_cast<int>((tmp & mask::day)    >> shift::day),
				static_cast<int>((tmp & mask::month)  >> shift::month),
				static_cast<int>(year_convert((tmp & mask::year) >> shift::year, lzx::packer)),
				static_cast<int>(day_of_week(tm.tm_year + 1900, tm.tm_mon, tm.tm_mday)),
				static_cast<int>(day_of_year(tm.tm_year + 1900, tm.tm_mon, tm.tm_mday))
			};

			return mktime(&tm);
		};

		uint32_t from_stdc(time_t t)
		{
			struct tm tm;
			localtime_r(&t, &tm);
			tm.tm_year += 1900;

			return (((tm.tm_sec   << shift::second) & mask::second) +
			        ((tm.tm_min   << shift::minute) & mask::minute) +
			        ((tm.tm_hour  << shift::hour)   & mask::hour)   +
			        ((tm.tm_mday  << shift::day)    & mask::day)    +
			        ((tm.tm_mon   << shift::month)  & mask::month)  +
			        ((tm.tm_year  << shift::year)   & mask::year));
		}

		void to_file(const char* filename, time_t timestamp)
		{
			struct utimbuf date = { timestamp, timestamp };
			utime(filename, &date);
		}

		void print(time_t t)
		{
			struct tm tm;
			static char time_str[32];
			static char date_str[32];
			localtime_r(&t, &tm);
			strftime(time_str, 32, "%H:%M:%S", &tm);
			strftime(date_str, 32, "%d-%b-%Y", &tm);
			printf("%s %s ", time_str, date_str);
		}
	};

	namespace checksum
	{
		namespace crc32
		{
			using type = uint32_t;
			inline constexpr uint32_t _mm_crc32_0xedb88320_u8(uint32_t crc, uint8_t v)
			{
				crc ^= v;
				static const uint32_t crc32_half_byte_tbl[] = {
					0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4,
					0x4db26158, 0x5005713c, 0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
					0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
				};

				crc = (crc >> 4) ^ crc32_half_byte_tbl[crc & 0x0F];
				crc = (crc >> 4) ^ crc32_half_byte_tbl[crc & 0x0F];
				return crc;
			}

			uint32_t calc(uint32_t crc = 0, const uint8_t* mem = NULL, uint32_t len = 0)
			{
				crc = ~crc;
				while(len--)
					crc = _mm_crc32_0xedb88320_u8(crc, *mem++);

				return ~crc;
			}
		};
	};
	struct info
	{
		enum flag : uint8_t
		{
			damage_protect = 1,
			locked         = 2,
		};
		std::array<uint8_t, 3> id;
		enum flag flags;
		std::array<uint8_t, 6> data;
	};

	struct header
	{
		enum prot
		{
			read      = 1 << 0,
			write     = 1 << 1,
			erase     = 1 << 2,
			executive = 1 << 3,
			archive   = 1 << 4,
			hold      = 1 << 5,
			script    = 1 << 6,
			pure      = 1 << 7,
		};
		enum os
		{
			msdos     = 0 << 0,
			windows   = 1 << 0,
			os2       = 1 << 1,
			amiga     = 10,
			unix_like = 20,
		};
		enum pack
		{
			merged    = 1 << 0,
			store     = 0 << 0,
			normal    = 1 << 1,
			eof       = 1 << 5,
		};

		uint8_t         attributes;
		uint8_t         unknown0;
		uint32_t        len_unpacked;
		uint32_t        len_packed;
		uint8_t         machine_type;
		uint8_t         pack_mode;
		uint8_t         flags;
		uint8_t         unknown1;
		uint8_t         len_comment; // [0,79]
		uint8_t         extract_ver;
		uint8_t         unknown2;
		uint8_t         unknown3;
		time::packed    date;
		checksum::crc32::type data_crc;
		checksum::crc32::type header_crc;
		uint8_t         len_filename;
	};

	struct filename_node
	{
		struct filename_node *next;
		uint32_t length;
		uint32_t crc;
		char filename[1 << 8];
	};

	struct file
	{
		std::array<uint8_t, 10>  info_header;
		std::array<uint8_t, 31>  archive_header;
		std::array<uint8_t, 256> header_filename;
		std::array<uint8_t, 256> header_comment;
		std::vector<struct filename_node> filename_list;
	};


	static std::array<const uint8_t,32> table_one =
	{
		0,0,0,0,
		1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13,14,14
	};

	static std::array<const uint32_t, 32> table_two =
	{
		0,1,2,3,4,6,8,12,16,24,32,48,64,96,128,192,256,384,512,768,1024,
		1536,2048,3072,4096,6144,8192,12288,16384,24576,32768,49152
	};

	template<typename Container, const uint32_t... I>
	static constexpr Container make_array_from_seq_2p1(std::integer_sequence<const uint32_t, I...>)
	{
		return { ((1 << I) - 1)... };
	}

	template<typename Container, const uint8_t... I>
	static constexpr Container make_array_from_seq(std::integer_sequence<const uint8_t, I...>)
	{
		return { I... };
	}

	static const std::array<const uint32_t, 16> table_three = make_array_from_seq_2p1<std::array<const uint32_t, 16>>(std::make_integer_sequence<const uint32_t, 16>{});
	static const std::array<const  uint8_t, 17> table_four  = make_array_from_seq<std::array<const uint8_t, 17>>(std::make_integer_sequence<const uint8_t, 17>{}); 

};


