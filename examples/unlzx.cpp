/* $VER: unlzx.c 1.0 (22.2.98) */
/* Created: 11.2.98 */
/* Added Pipe support to read from stdin (03.4.01, Erik Meusel)           */

/* LZX Extract in (supposedly) portable C.                                */

/* Compile with:                                                          */
/* gcc unlzx.c -ounlzx -O6                                                */

/* Thanks to Dan Fraser for decoding the coredumps and helping me track   */
/* down some HIDEOUSLY ANNOYING bugs.                                     */

/* Everything is accessed as unsigned char's to try and avoid problems    */
/* with byte order and alignment. Most of the decrunch functions          */
/* encourage overruns in the buffers to make things as fast as possible.  */
/* All the time is taken up in lzx::checksum::crc32::calc() and decrunch() so they are      */
/* pretty damn optimized. Don't try to understand this program.           */

/* ---------------------------------------------------------------------- */

#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <cstdbool>
#include <cstddef>
#include <ctime>
#include <cmath>
#include <cstring>
#include <array>
#include <utility>
#include <fcntl.h>
#include <sys/stat.h>
#include <utime.h>
#include <getopt.h>
#include <lzx/lzx.h>

/* ---------------------------------------------------------------------- */

static const unsigned char VERSION[]="$VER: unlzx 1.1 (03.4.01)";

/* ---------------------------------------------------------------------- */


int mode;

unsigned char info_header[10];
unsigned char archive_header[31];
unsigned char header_filename[256];
unsigned char header_comment[256];

uint32_t pack_size;
uint32_t unpack_size;

uint32_t crc;

unsigned char attributes;
unsigned char pack_mode;

/* ---------------------------------------------------------------------- */

struct filename_node
{
 struct filename_node *next;
 uint32_t length;
 uint32_t crc;
 char filename[1 << 8];
};

struct filename_node *filename_list;

/* ---------------------------------------------------------------------- */

unsigned char read_buffer[1 << 14]; /* have a reasonable sized read buffer */
unsigned char decrunch_buffer[258 + (1 << 16) + 258]; /* allow overrun for speed */

unsigned char* source;
unsigned char* destination;
unsigned char* source_end;
unsigned char* destination_end;

uint32_t decrunch_method;
uint32_t decrunch_length;
uint32_t last_offset;
uint32_t global_control;
int global_shift;

unsigned char offset_len[8];
uint16_t offset_table[128];
unsigned char huffman20_len[20];
uint16_t huffman20_table[96];
unsigned char literal_len[768];
uint16_t literal_table[5120];

uint32_t sum;


/* ---------------------------------------------------------------------- */

/* Build a fast huffman decode table from the symbol bit lengths.         */
/* There is an alternate algorithm which is faster but also more complex. */

int make_decode_table(int number_symbols, int table_size,
		unsigned char *length, uint16_t *table)
{
	unsigned char bit_num = 0;
	int symbol;
	uint32_t leaf; /* could be a register */
	uint32_t table_mask, bit_mask, pos, fill, next_symbol, reverse;
	int abort = 0;

	pos = 0; /* consistantly used as the current position in the decode table */

	bit_mask = table_mask = 1 << table_size;

	bit_mask >>= 1; /* don't do the first number */
	bit_num++;

	while((!abort) && (bit_num <= table_size))
	{
		for(symbol = 0; symbol < number_symbols; symbol++)
		{
			if(length[symbol] == bit_num)
			{
				reverse = pos; /* reverse the order of the position's bits */
				leaf = 0;
				fill = table_size;
				do /* reverse the position */
				{
					leaf = (leaf << 1) + (reverse & 1);
					reverse >>= 1;
				} while(--fill);
				if((pos += bit_mask) > table_mask)
				{
					abort = 1;
					break; /* we will overrun the table! abort! */
				}
				fill = bit_mask;
				next_symbol = 1 << bit_num;
				do
				{
					table[leaf] = symbol;
					leaf += next_symbol;
				} while(--fill);
			}
		}
		bit_mask >>= 1;
		bit_num++;
	}

	if((!abort) && (pos != table_mask))
	{
		for(symbol = pos; symbol < table_mask; symbol++) /* clear the rest of the table */
		{
			reverse = symbol; /* reverse the order of the position's bits */
			leaf = 0;
			fill = table_size;
			do /* reverse the position */
			{
				leaf = (leaf << 1) + (reverse & 1);
				reverse >>= 1;
			} while(--fill);
			table[leaf] = 0;
		}
		next_symbol = table_mask >> 1;
		pos <<= 16;
		table_mask <<= 16;
		bit_mask = 32768;

		while((!abort) && (bit_num <= 16))
		{
			for(symbol = 0; symbol < number_symbols; symbol++)
			{
				if(length[symbol] == bit_num)
				{
					reverse = pos >> 16; /* reverse the order of the position's bits */
					leaf = 0;
					fill = table_size;
					do /* reverse the position */
					{
						leaf = (leaf << 1) + (reverse & 1);
						reverse >>= 1;
					} while(--fill);
					for(fill = 0; fill < bit_num - table_size; fill++)
					{
						if(table[leaf] == 0)
						{
							table[(next_symbol << 1)] = 0;
							table[(next_symbol << 1) + 1] = 0;
							table[leaf] = next_symbol++;
						}
						leaf = table[leaf] << 1;
						leaf += (pos >> (15 - fill)) & 1;
					}
					table[leaf] = symbol;
					if((pos += bit_mask) > table_mask)
					{
						abort = 1;
						break; /* we will overrun the table! abort! */
					}
				}
			}
			bit_mask >>= 1;
			bit_num++;
		}
	}
	if(pos != table_mask) abort = 1; /* the table is incomplete! */

	return(abort);
}

/* ---------------------------------------------------------------------- */

/* Read and build the decrunch tables. There better be enough data in the */
/* source buffer or it's stuffed. */

int read_literal_table()
{
	uint32_t control;
	int shift;
	uint32_t temp; /* could be a register */
	uint32_t symbol, pos, count, fix, max_symbol;
	int abort = 0;

	control = global_control;
	shift = global_shift;

	if(shift < 0) /* fix the control word if necessary */
	{
		shift += 16;
		control += *source++ << (8 + shift);
		control += *source++ << shift;
	}

	/* read the decrunch method */

	decrunch_method = control & 7;
	control >>= 3;
	if((shift -= 3) < 0)
	{
		shift += 16;
		control += *source++ << (8 + shift);
		control += *source++ << shift;
	}

	/* Read and build the offset huffman table */

	if((!abort) && (decrunch_method == 3))
	{
		for(temp = 0; temp < 8; temp++)
		{
			offset_len[temp] = control & 7;
			control >>= 3;
			if((shift -= 3) < 0)
			{
				shift += 16;
				control += *source++ << (8 + shift);
				control += *source++ << shift;
			}
		}
		abort = make_decode_table(8, 7, offset_len, offset_table);
	}

	/* read decrunch length */

	if(!abort)
	{
		decrunch_length = (control & 255) << 16;
		control >>= 8;
		if((shift -= 8) < 0)
		{
			shift += 16;
			control += *source++ << (8 + shift);
			control += *source++ << shift;
		}
		decrunch_length += (control & 255) << 8;
		control >>= 8;
		if((shift -= 8) < 0)
		{
			shift += 16;
			control += *source++ << (8 + shift);
			control += *source++ << shift;
		}
		decrunch_length += (control & 255);
		control >>= 8;
		if((shift -= 8) < 0)
		{
			shift += 16;
			control += *source++ << (8 + shift);
			control += *source++ << shift;
		}
	}

	/* read and build the huffman literal table */

	if((!abort) && (decrunch_method != 1))
	{
		pos = 0;
		fix = 1;
		max_symbol = 256;

		do
		{
			for(temp = 0; temp < 20; temp++)
			{
				huffman20_len[temp] = control & 15;
				control >>= 4;
				if((shift -= 4) < 0)
				{
					shift += 16;
					control += *source++ << (8 + shift);
					control += *source++ << shift;
				}
			}
			abort = make_decode_table(20, 6, huffman20_len, huffman20_table);

			if(abort) break; /* argh! table is corrupt! */

			do
			{
				if((symbol = huffman20_table[control & 63]) >= 20)
				{
					do /* symbol is longer than 6 bits */
					{
						symbol = huffman20_table[((control >> 6) & 1) + (symbol << 1)];
						if(!shift--)
						{
							shift += 16;
							control += *source++ << 24;
							control += *source++ << 16;
						}
						control >>= 1;
					} while(symbol >= 20);
					temp = 6;
				}
				else
				{
					temp = huffman20_len[symbol];
				}
				control >>= temp;
				if((shift -= temp) < 0)
				{
					shift += 16;
					control += *source++ << (8 + shift);
					control += *source++ << shift;
				}
				switch(symbol)
				{
					case 17:
					case 18:
						{
							if(symbol == 17)
							{
								temp = 4;
								count = 3;
							}
							else /* symbol == 18 */
							{
								temp = 6 - fix;
								count = 19;
							}
							count += (control & lzx::table_three[temp]) + fix;
							control >>= temp;
							if((shift -= temp) < 0)
							{
								shift += 16;
								control += *source++ << (8 + shift);
								control += *source++ << shift;
							}
							while((pos < max_symbol) && (count--))
								literal_len[pos++] = 0;
							break;
						}
					case 19:
						{
							count = (control & 1) + 3 + fix;
							if(!shift--)
							{
								shift += 16;
								control += *source++ << 24;
								control += *source++ << 16;
							}
							control >>= 1;
							if((symbol = huffman20_table[control & 63]) >= 20)
							{
								do /* symbol is longer than 6 bits */
								{
									symbol = huffman20_table[((control >> 6) & 1) + (symbol << 1)];
									if(!shift--)
									{
										shift += 16;
										control += *source++ << 24;
										control += *source++ << 16;
									}
									control >>= 1;
								} while(symbol >= 20);
								temp = 6;
							}
							else
							{
								temp = huffman20_len[symbol];
							}
							control >>= temp;
							if((shift -= temp) < 0)
							{
								shift += 16;
								control += *source++ << (8 + shift);
								control += *source++ << shift;
							}
							symbol = lzx::table_four[(literal_len[pos] + 17 - symbol) % 17];
							while((pos < max_symbol) && (count--))
								literal_len[pos++] = symbol;
							break;
						}
					default:
						{
							symbol = lzx::table_four[(literal_len[pos] + 17 - symbol) % 17];
							literal_len[pos++] = symbol;
							break;
						}
				}
			} while(pos < max_symbol);
			fix--;
			max_symbol += 512;
		} while(max_symbol == 768);

		if(!abort)
			abort = make_decode_table(768, 12, literal_len, literal_table);
	}

	global_control = control;
	global_shift = shift;

	return(abort);
}

/* ---------------------------------------------------------------------- */

/* Fill up the decrunch buffer. Needs lots of overrun for both destination */
/* and source buffers. Most of the time is spent in this routine so it's  */
/* pretty damn optimized. */

void decrunch()
{
	uint32_t control;
	int shift;
	uint32_t temp; /* could be a register */
	uint32_t symbol, count;
	unsigned char *string;

	control = global_control;
	shift = global_shift;

	do
	{
		if((symbol = literal_table[control & 4095]) >= 768)
		{
			control >>= 12;
			if((shift -= 12) < 0)
			{
				shift += 16;
				control += *source++ << (8 + shift);
				control += *source++ << shift;
			}
			do /* literal is longer than 12 bits */
			{
				symbol = literal_table[(control & 1) + (symbol << 1)];
				if(!shift--)
				{
					shift += 16;
					control += *source++ << 24;
					control += *source++ << 16;
				}
				control >>= 1;
			} while(symbol >= 768);
		}
		else
		{
			temp = literal_len[symbol];
			control >>= temp;
			if((shift -= temp) < 0)
			{
				shift += 16;
				control += *source++ << (8 + shift);
				control += *source++ << shift;
			}
		}
		if(symbol < 256)
		{
			*destination++ = symbol;
		}
		else
		{
			symbol -= 256;
			count = lzx::table_two[temp = symbol & 31];
			temp = lzx::table_one[temp];
			if((temp >= 3) && (decrunch_method == 3))
			{
				temp -= 3;
				count += ((control & lzx::table_three[temp]) << 3);
				control >>= temp;
				if((shift -= temp) < 0)
				{
					shift += 16;
					control += *source++ << (8 + shift);
					control += *source++ << shift;
				}
				count += (temp = offset_table[control & 127]);
				temp = offset_len[temp];
			}
			else
			{
				count += control & lzx::table_three[temp];
				if(!count) count = last_offset;
			}
			control >>= temp;
			if((shift -= temp) < 0)
			{
				shift += 16;
				control += *source++ << (8 + shift);
				control += *source++ << shift;
			}
			last_offset = count;

			count = lzx::table_two[temp = (symbol >> 5) & 15] + 3;
			temp = lzx::table_one[temp];
			count += (control & lzx::table_three[temp]);
			control >>= temp;
			if((shift -= temp) < 0)
			{
				shift += 16;
				control += *source++ << (8 + shift);
				control += *source++ << shift;
			}
			string = (decrunch_buffer + last_offset < destination) ?
				destination - last_offset : destination + 65536 - last_offset;
			do
			{
				*destination++ = *string++;
			} while(--count);
		}
	} while((destination < destination_end) && (source < source_end));

	global_control = control;
	global_shift = shift;
}

/* ---------------------------------------------------------------------- */

/* Opens a file for writing & creates the full path if required. */

FILE *open_output(char *filename)
{
	uint32_t temp;
	FILE *file;

	if(!(file = fopen(filename, "wb")))
	{
		/* couldn't open the file. try and create directories */
		for(temp = 0; filename[temp]; temp++)
		{
			if(filename[temp] == '/')
			{
				filename[temp] = 0;
				mkdir(filename, 511); /* I don't care if it works or not */
				filename[temp] = '/';
			}
		}
		if(!(file = fopen(filename, "wb")))
		{
			perror("FOpen");
		}
	}
	return(file);
}

/* ---------------------------------------------------------------------- */

/* Trying to understand this function is hazardous. */

int extract_normal(FILE *in_file)
{
	struct filename_node *node;
	FILE *out_file = 0;
	unsigned char *pos;
	unsigned char *temp;
	uint32_t count;
	int abort = 0;

	global_control = 0; /* initial control word */
	global_shift = -16;
	last_offset = 1;
	unpack_size = 0;
	decrunch_length = 0;

	for(count = 0; count < 8; count++)
		offset_len[count] = 0;
	for(count = 0; count < 768; count ++)
		literal_len[count] = 0;

	source_end = (source = read_buffer + 16384) - 1024;
	pos = destination_end = destination = decrunch_buffer + 258 + 65536;

	for(node = filename_list; (!abort) && node; node = node->next)
	{
		printf("Extracting \"%s\"...", node->filename);
		fflush(stdout);

		out_file = open_output(node->filename);

		sum = 0; /* reset CRC */

		unpack_size = node->length;

		while(unpack_size > 0)
		{

			if(pos == destination) /* time to fill the buffer? */
			{
				/* check if we have enough data and read some if not */
				if(source >= source_end) /* have we exhausted the current read buffer? */
				{
					temp = read_buffer;
					if(count = temp - source + 16384)
					{
						do /* copy the remaining overrun to the start of the buffer */
						{
							*temp++ = *source++;
						} while(--count);
					}
					source = read_buffer;
					count = source - temp + 16384;

					if(pack_size < count) count = pack_size; /* make sure we don't read too much */

					if(fread(temp, 1, count, in_file) != count)
					{
						printf("\n");
						if(ferror(in_file))
							perror("FRead(Data)");
						else
							fprintf(stderr, "EOF: Data\n");
						abort = 1;
						break; /* fatal error */
					}
					pack_size -= count;

					temp += count;
					if(source >= temp) break; /* argh! no more data! */
				} /* if(source >= source_end) */

				/* check if we need to read the tables */
				if(decrunch_length <= 0)
				{
					if(read_literal_table()) break; /* argh! can't make huffman tables! */
				}

				/* unpack some data */
				if(destination >= decrunch_buffer + 258 + 65536)
				{
					if(count = destination - decrunch_buffer - 65536)
					{
						temp = (destination = decrunch_buffer) + 65536;
						do /* copy the overrun to the start of the buffer */
						{
							*destination++ = *temp++;
						} while(--count);
					}
					pos = destination;
				}
				destination_end = destination + decrunch_length;
				if(destination_end > decrunch_buffer + 258 + 65536)
					destination_end = decrunch_buffer + 258 + 65536;
				temp = destination;

				decrunch();

				decrunch_length -= (destination - temp);
			}

			/* calculate amount of data we can use before we need to fill the buffer again */
			count = destination - pos;
			if(count > unpack_size) count = unpack_size; /* take only what we need */

			sum = lzx::checksum::crc32::calc(sum, pos, count);

			if(out_file) /* Write the data to the file */
			{
				if(fwrite(pos, 1, count, out_file) != count)
				{
					perror("FWrite"); /* argh! write error */
					fclose(out_file);
					out_file = 0;
				}
			}
			unpack_size -= count;
			pos += count;
		}
		lzx::time::to_file(node->filename, lzx::time::to_stdc(&archive_header[18]));

		if(out_file)
		{
			fclose(out_file);
			if(!abort) printf(" crc %s\n", (node->crc == sum) ? "good" : "bad");
		}
	} /* for */

	return(abort);
}

/* ---------------------------------------------------------------------- */

/* This is less complex than extract_normal. Almost decipherable. */

int extract_store(FILE *in_file)
{
	struct filename_node *node;
	FILE *out_file;
	uint32_t count;
	int abort = 0;

	for(node = filename_list; (!abort) && node; node = node->next)
	{
		printf("Storing \"%s\"...", node->filename);
		fflush(stdout);

		out_file = open_output(node->filename);

		sum = 0; /* reset CRC */

		unpack_size = node->length;
		if(unpack_size > pack_size) unpack_size = pack_size;

		while(unpack_size > 0)
		{
			count = (unpack_size > 16384) ? 16384 : unpack_size;

			if(fread(read_buffer, 1, count, in_file) != count)
			{
				printf("\n");
				if(ferror(in_file))
					perror("FRead(Data)");
				else
					fprintf(stderr, "EOF: Data\n");
				abort = 1;
				break; /* fatal error */
			}
			pack_size -= count;

			sum = lzx::checksum::crc32::calc(sum, read_buffer, count);

			if(out_file) /* Write the data to the file */
			{
				if(fwrite(read_buffer, 1, count, out_file) != count)
				{
					perror("FWrite"); /* argh! write error */
					fclose(out_file);
					out_file = 0;
				}
			}
			unpack_size -= count;
		}
		lzx::time::to_file(node->filename, lzx::time::to_stdc(&archive_header[18]));

		if(out_file)
		{
			fclose(out_file);
			if(!abort) printf(" crc %s\n", (node->crc == sum) ? "good" : "bad");
		}
	} /* for */

	return(abort);
}

/* ---------------------------------------------------------------------- */

/* Easiest of the three. Just print the file(s) we didn't understand. */

int extract_unknown(FILE *in_file)
{
	struct filename_node *node;
	int abort = 0;

	for(node = filename_list; node; node = node->next)
	{
		printf("Unknown \"%s\"\n", node->filename);
	}

	return(abort);
}

/* ---------------------------------------------------------------------- */

/* Read the archive and build a linked list of names. Merged files is     */
/* always assumed. Will fail if there is no memory for a node. Sigh.      */

int extract_archive(FILE *in_file)
{
	uint32_t temp;
	struct filename_node **filename_next;
	struct filename_node *node;
	struct filename_node *temp_node;
	int actual;
	int abort;
	int result = EXIT_FAILURE; /* assume an error */

	filename_list = 0; /* clear the list */
	filename_next = &filename_list;

	do
	{
		abort = 1; /* assume an error */
		actual = fread(archive_header, 1, 31, in_file);
		if(!ferror(in_file))
		{
			if(actual) /* 0 is normal and means EOF */
			{
				if(actual == 31)
				{
					sum = 0; /* reset CRC */
					crc = (archive_header[29] << 24) + (archive_header[28] << 16) + (archive_header[27] << 8) + archive_header[26]; /* header crc */
					archive_header[29] = 0; /* Must set the field to 0 before calculating the crc */
					archive_header[28] = 0;
					archive_header[27] = 0;
					archive_header[26] = 0;
					sum = lzx::checksum::crc32::calc(sum, archive_header, 31);
					temp = archive_header[30]; /* filename length */
					actual = fread(header_filename, 1, temp, in_file);
					if(!ferror(in_file))
					{
						if(actual == temp)
						{
							header_filename[temp] = 0;
							sum = lzx::checksum::crc32::calc(sum, header_filename, temp);
							temp = archive_header[14]; /* comment length */
							actual = fread(header_comment, 1, temp, in_file);
							if(!ferror(in_file))
							{
								if(actual == temp)
								{
									header_comment[temp] = 0;
									sum = lzx::checksum::crc32::calc(sum, header_comment, temp);
									if(sum == crc)
									{
										unpack_size = (archive_header[5] << 24) + (archive_header[4] << 16) + (archive_header[3] << 8) + archive_header[2]; /* unpack size */
										pack_size = (archive_header[9] << 24) + (archive_header[8] << 16) + (archive_header[7] << 8) + archive_header[6]; /* packed size */
										pack_mode = archive_header[11]; /* pack mode */
										crc = (archive_header[25] << 24) + (archive_header[24] << 16) + (archive_header[23] << 8) + archive_header[22]; /* data crc */

										if(node = (struct filename_node *)malloc(sizeof(struct filename_node))) /* allocate a filename node */
										{
											*filename_next = node; /* add this node to the list */
											filename_next = &(node->next);
											node->next = 0;
											node->length = unpack_size;
											node->crc = crc;
											for(temp = 0; node->filename[temp] = header_filename[temp]; temp++);

												switch(pack_mode)
												{
													case 0: /* store */
														{
															abort = extract_store(in_file);
															break;
														}
													case 2: /* normal */
														{
															abort = extract_normal(in_file);
															break;
														}
													default: /* unknown */
														{
															abort = extract_unknown(in_file);
															break;
														}
												}
												if(abort) break; /* a read error occured */

												temp_node = filename_list; /* free the list now */
												while(node = temp_node)
												{
													temp_node = node->next;
													free(node);
												}
												filename_list = 0; /* clear the list */
												filename_next = &filename_list;

												if(fseek(in_file, pack_size, SEEK_CUR))
												{
													perror("FSeek(Data)");
													break;
												}
										}
										else
											fprintf(stderr, "MAlloc(Filename_node)\n");
									}
									else
										fprintf(stderr, "CRC: Archive_Header\n");
								}
								else
									fprintf(stderr, "EOF: Header_Comment\n");
							}
							else
								perror("FRead(Header_Comment)");
						}
						else
							fprintf(stderr, "EOF: Header_Filename\n");
					}
					else
						perror("FRead(Header_Filename)");
				}
				else
					fprintf(stderr, "EOF: Archive_Header\n");
			}
			else
			{
				result = EXIT_SUCCESS; /* normal termination */
			}
		}
		else
			perror("FRead(Archive_Header)");
	} while(!abort);

	/* free the filename list in case an error occured */
	temp_node = filename_list;
	while(node = temp_node)
	{
		temp_node = node->next;
		free(node);
	}

	return(result);
}

/* ---------------------------------------------------------------------- */

/* List the contents of an archive in a nice formatted kinda way.         */

int view_archive(FILE *in_file)
{
 uint32_t temp;
 uint32_t total_pack = 0;
 uint32_t total_unpack = 0;
 uint32_t total_files = 0;
 uint32_t merge_size = 0;
 int actual;
 int abort;
 int result = 1; /* assume an error */

 printf("Unpacked   Packed Time     Date        Attrib   Name\n");
 printf("-------- -------- -------- ----------- -------- ----\n");

 do
 {
	 abort = 1; /* assume an error */
	 actual = fread(archive_header, 1, 31, in_file);
	 if(!ferror(in_file))
	 {
		 if(actual) /* 0 is normal and means EOF */
		 {
			 if(actual == 31)
			 {
				 sum = 0; /* reset CRC */
				 crc = (archive_header[29] << 24) + (archive_header[28] << 16) + (archive_header[27] << 8) + archive_header[26];
				 archive_header[29] = 0; /* Must set the field to 0 before calculating the crc */
				 archive_header[28] = 0;
				 archive_header[27] = 0;
				 archive_header[26] = 0;
				 sum = lzx::checksum::crc32::calc(sum, archive_header, 31);
				 temp = archive_header[30]; /* filename length */
				 actual = fread(header_filename, 1, temp, in_file);
				 if(!ferror(in_file))
				 {
					 if(actual == temp)
					 {
						 header_filename[temp] = 0;
						 sum = lzx::checksum::crc32::calc(sum, header_filename, temp);
						 temp = archive_header[14]; /* comment length */
						 actual = fread(header_comment, 1, temp, in_file);
						 if(!ferror(in_file))
						 {
							 if(actual == temp)
							 {
								 header_comment[temp] = 0;
								 sum = lzx::checksum::crc32::calc(sum, header_comment, temp);
								 if(sum == crc)
								 {
									 attributes = archive_header[0]; /* file protection modes */
									 unpack_size = (archive_header[5] << 24) + (archive_header[4] << 16) + (archive_header[3] << 8) + archive_header[2]; /* unpack size */
									 pack_size = (archive_header[9] << 24) + (archive_header[8] << 16) + (archive_header[7] << 8) + archive_header[6]; /* packed size */

									 total_pack += pack_size;
									 total_unpack += unpack_size;
									 total_files++;
									 merge_size += unpack_size;

									 printf("%8u ", unpack_size);

									 if(archive_header[12] & 1)
										 printf("     n/a ");
									 else
										 printf("%8u ", pack_size);

									 lzx::time::print(lzx::time::to_stdc(&archive_header[18]));

									 printf("%c%c%c%c%c%c%c%c ",
											 (attributes &  32) ? 'h' : '-',
											 (attributes &  64) ? 's' : '-',
											 (attributes & 128) ? 'p' : '-',
											 (attributes &  16) ? 'a' : '-',
											 (attributes &   1) ? 'r' : '-',
											 (attributes &   2) ? 'w' : '-',
											 (attributes &   8) ? 'e' : '-',
											 (attributes &   4) ? 'd' : '-');

									 printf("\"%s\"\n", header_filename);

									 if(header_comment[0])
										 printf(": \"%s\"\n", header_comment);
									 if((archive_header[12] & 1) && pack_size)
									 {
										 printf("%8u %8u Merged\n", merge_size, pack_size);
									 }

									 if(pack_size) /* seek past the packed data */
									 {
										 merge_size = 0;
										 if(!fseek(in_file, pack_size, SEEK_CUR))
										 {
											 abort = 0; /* continue */
										 }
										 else
											 perror("FSeek()");
									 }
									 else
										 abort = 0; /* continue */
								 }
								 else
									 fprintf(stderr, "CRC: Archive_Header\n");
							 }
							 else
								 fprintf(stderr, "EOF: Header_Comment\n");
						 }
						 else
							 perror("FRead(Header_Comment)");
					 }
					 else
						 fprintf(stderr, "EOF: Header_Filename\n");
				 }
				 else
					 perror("FRead(Header_Filename)");
			 }
			 else
				 fprintf(stderr, "EOF: Archive_Header\n");
		 }
		 else
		 {
			 printf("-------- -------- -------- ----------- -------- ----\n");
			 printf("%8u %8u ", total_unpack, total_pack);
			 printf("%u file%s\n", total_files, ((total_files == 1) ? "" : "s"));

			 result = 0; /* normal termination */
		 }
	 }
	 else
		 perror("FRead(Archive_Header)");
 } while(!abort);

 return(result);
}

/* ---------------------------------------------------------------------- */

/* Process a single archive. */

int process_archive(char *filename)
{
	int result = 1; /* assume an error */
	FILE *in_file;
	int actual;

	if(NULL == filename)
		in_file = stdin;
	else if(NULL == (in_file = fopen(filename,"rb")))
	{
		perror("FOpen(Archive)");
		return(result);
	}

	actual = fread(info_header, 1, 10, in_file);
	if(!ferror(in_file))
	{
		if(actual == 10)
		{
			if((info_header[0] == 76) && (info_header[1] == 90) && (info_header[2] == 88)) /* LZX */
			{
				switch(mode)
				{
					case 1: /* extract archive */
						{
							result = extract_archive(in_file);
							break;
						}
					case 2: /* view archive */
						{
							result = view_archive(in_file);
							break;
						}
				}
			}
			else
				fprintf(stderr, "Info_Header: Bad ID\n");
		}
		else
			fprintf(stderr, "EOF: Info_Header\n");
	}
	else
		perror("FRead(Info_Header)");
	fclose(in_file);

	return(result);
}

/* ---------------------------------------------------------------------- */

/* Handle options & multiple filenames. */

int main(int argc, char **argv)
{
	int result = EXIT_SUCCESS;
	int read_from_stdin = 0;
	int option;
	extern int optind;

	mode = 1; /* default mode is extract */
	while ((option = getopt(argc, argv, "vxcp:")) != EOF)
	{
		switch(option)
		{
			case 'p': /* packer variant */
				{
					if(strncmp(optarg, "titov", 5) == 0)
						lzx::packer = lzx::packer::titov;
					else if(strncmp(optarg, "calusinksi", 10) == 0)
						lzx::packer = lzx::packer::calusinski;
				}
			case 'v': /* (v)iew archive */
				{
					mode = 2;
					break;
				}
			case 'x': /* e(x)tract archive */
				{
					mode = 1;
					break;
				}
			case 'c': /* use stdin to extract/view from */
				{
					read_from_stdin = 1;
					break;
				}
			case '?': /* unknown option */
				{
					result = EXIT_FAILURE;
					break;
				}
		}
	}
	if(!read_from_stdin && optind >= argc) result = EXIT_FAILURE;
	/* gotta have a filename or read from stdin */

	if(result != EXIT_FAILURE)
	{
		if (read_from_stdin)
		{
			printf("\nReading from stdin...\n\n");
			process_archive(NULL);
			result = 0;
		}
		if((argc - optind) > 1)
		{
			for(; optind < argc; optind++)
			{
				printf("\nArchive \"%s\"...\n\n", argv[optind]);
				process_archive(argv[optind]);
			}
			result = 0; /* Can't give a reliable result for multiple archives */
		}
		else
		{
			result = process_archive(argv[optind]); /* do a single archive */
		}
	}
	else
	{
		fprintf(stderr, "Usage: unlzx [-p TITOV/CALUSINKSI][-v][-x][-c] [archive...]\n");
		fprintf(stderr, "\t-p : choose packers Y2K style used");
		fprintf(stderr, "\t-c : extract/list from stdin\n");
		fprintf(stderr, "\t-v : list archive(s)\n");
		fprintf(stderr, "\t-x : extract (default)\n");
		result = 2;
	}

	exit(result);
}
