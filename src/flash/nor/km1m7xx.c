/***************************************************************************
 * (C) Nuvoton Technology Corporation Japan 2021                           *
 *   yamaguchi.yoshikazu@nuvoton.com                                       *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>. *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "imp.h"
#include <helper/binarybuffer.h>
#include <target/algorithm.h>
#include <target/armv7m.h>

#include <target/image.h>

/* Definition for Flash Memory */
#define	FLASH_SECTOR_SIZE		0x1000

/* Definition for Flash Memory Interface Register */
#define	FI_BASE_ADDRESS			0x4001C000

#define	FEWEN					0x4001C000
#define	FEWEN_KEY_CODE			0x2900
#define	FEWEN_ENABLE			0x004B

#define	SPROSTR					0x4001C018
#define	SPROSTR_ENABLE			0x00800000
#define	SPROEND					0x4001C01C
#define	SPROEND_ENABLE			0x10810000

#define	FWCNT					0x4001C004
#define	FWCNT_ERASE				0x00000002
#define	FWCNT_START				0x00000001

#define	FMON					0x4001C008
#define	FMON_ERROR				0x0000FF00
#define	FMON_WBUSY				0x00000001

#define	PEADR					0x4001C00C

#define	TIMEOUT_ERASE			100000

struct km1m7xx_flash_bank {
	int		probed;
};

/**
 * @brief	"flash bank" Command
 * @date	October, 2018
 * @note	[Usage]	flash bank $_FLASHNAME km1m7xx
 *					<Address> <size> <ChipWidth> <BusWidth> <Target> <Type>
 *						<Address>	: Flash memory base address
 *						<Size>		: Flash memory size
 *						<ChipWidth>	: Chip width in byte (Not use)
 *						<BusWidth>	: Bus width in byte (Not use)
 *						<Target>	: Target device (***.cpu)
 *						<Type>		: Write contorol type
 * @param
 * @return	int			ERROR_OK or the non-zero
 **/
FLASH_BANK_COMMAND_HANDLER(km1m7xx_flash_bank_command)
{
	struct km1m7xx_flash_bank	*km1m7xx_info;

	km1m7xx_info = malloc(sizeof(struct km1m7xx_flash_bank));
	bank->driver_priv = km1m7xx_info;

	km1m7xx_info->probed	= 0;

	return(ERROR_OK);
}

static int km1m7xx_erase(struct flash_bank *bank, unsigned int first, unsigned int last)
{
	uint32_t	read_fwcnt		= 0;
	uint32_t	read_fmon		= 0;
	uint32_t	timeout			= 0;
	uint32_t	sector_index	= 0;
	uint32_t	address			= 0;

	/* Flash memory write enable */
	target_write_u32(bank->target, FEWEN,	(FEWEN_KEY_CODE | FEWEN_ENABLE));
	target_write_u32(bank->target, SPROSTR,	SPROSTR_ENABLE);
	target_write_u32(bank->target, SPROEND,	SPROEND_ENABLE);

	/* Erase specified sectors */
	for (sector_index = first; sector_index <= last; sector_index++) {

		/* Get sector addres */
		address = bank->base + bank->sectors[sector_index].offset;
		LOG_INFO("Erase at 0x%08x (Index:%d) \n", address, sector_index);

		/* Set parameter */
		target_write_u32(bank->target, PEADR,
						(bank->base + bank->sectors[sector_index].offset));

		/* Start erase */
		target_write_u32(bank->target, FWCNT, (FWCNT_ERASE | FWCNT_START));

		/* Read FMON three times to wait for FMON.BUSY to be set. */
		target_read_u32(bank->target, FMON, &read_fmon);
		target_read_u32(bank->target, FMON, &read_fmon);
		target_read_u32(bank->target, FMON, &read_fmon);

		/* Wait for erase completion */
		target_read_u32(bank->target, FMON, &read_fmon);
		read_fmon &= 0xFFFF;
		timeout = GetTickCount();
		while (1) {

			/* Check for completion */
			target_read_u32(bank->target, FMON, &read_fmon);
			if ((read_fmon & FMON_WBUSY) == 0x0000) {
				break;
			}

			/* Check error */
			if ((read_fmon & FMON_ERROR) != 0) {
				LOG_DEBUG("km1m7xx_erase() Error : FMON = %d\n", read_fmon);
				return(ERROR_FAIL);
			}

			/* Check timeout */
			if ((GetTickCount() - timeout) > TIMEOUT_ERASE) {
				LOG_DEBUG("km1m7xx_erase() timeout : FMON = %d\n", read_fmon);
				return(ERROR_FAIL);
			}
		}

		/* Clear START bit of FWCNT */
		target_read_u32(bank->target, FWCNT, &read_fwcnt);
		read_fwcnt &= ~(FWCNT_ERASE | FWCNT_START);
		target_write_u32(bank->target, FWCNT, read_fwcnt);

		/* Check error */
		if ((read_fmon & FMON_ERROR) != 0) {
			LOG_DEBUG("km1m7xx_erase() Error : FMON = %d\n", read_fmon);
			return(ERROR_FAIL);
		}
	}

	return(ERROR_OK);
}

static int km1m7xx_write(struct flash_bank *bank, const uint8_t *buffer, uint32_t offset, uint32_t count)
{

	int						result			= ERROR_OK;
	struct target			*target			= bank->target;
	struct working_area		*algorithm		= NULL;
	struct working_area		*source			= NULL;
	struct armv7m_algorithm	armv7m_info;

	struct reg_param		reg_params[2];
	uint32_t				mem_params32[4]	= {0, 0, 0, 0};
	uint8_t					mem_params8[sizeof(mem_params32)];

	uint32_t				remain_size		= 0;
	uint32_t				buffer_size		= 0;
	uint32_t				write_address	= 0;
	uint32_t				write_size		= 0;
	uint8_t					*write_data		= 0;
	uint32_t				status			= 0;

	static const uint8_t km1m7xx_write_code[] = {
		0x70, 0xB5, 0x00, 0x22, 0x00, 0x20, 0x00, 0x23,
		0x00, 0x21, 0x00, 0x24, 0x1F, 0x4D, 0x4D, 0x44,
		0x2A, 0x68, 0x1F, 0x4D, 0x4D, 0x44, 0x28, 0x68,
		0x1E, 0x4D, 0x4D, 0x44, 0x2B, 0x68, 0x00, 0x25,
		0x1D, 0x4E, 0x4E, 0x44, 0x35, 0x60, 0x2B, 0xE0,
		0x1C, 0x4D, 0xEA, 0x60, 0x20, 0xC8, 0x1B, 0x4E,
		0x35, 0x61, 0x20, 0xC8, 0x75, 0x61, 0x00, 0x25,
		0x35, 0x71, 0x01, 0x25, 0x35, 0x71, 0x35, 0x46,
		0x29, 0x89, 0x29, 0x89, 0x29, 0x89, 0x16, 0x4C,
		0x29, 0x89, 0x00, 0xBF, 0x25, 0x1E, 0xA4, 0xF1,
		0x01, 0x04, 0x00, 0xD1, 0x70, 0xBD, 0x11, 0x4D,
		0x29, 0x89, 0x01, 0xF0, 0x01, 0x05, 0x00, 0x2D,
		0xF4, 0xD1, 0x0E, 0x4D, 0x2D, 0x79, 0x25, 0xF0,
		0x01, 0x05, 0x0C, 0x4E, 0x35, 0x71, 0x01, 0xF4,
		0x7F, 0x45, 0x1D, 0xB1, 0x08, 0x4D, 0x4D, 0x44,
		0x29, 0x60, 0x03, 0xE0, 0x08, 0x32, 0x08, 0x3B,
		0x00, 0x2B, 0xD1, 0xD1, 0x00, 0xBF, 0x00, 0xBE,
		0x00, 0xBF, 0xE3, 0xE7, 0x44, 0x00, 0x00, 0x00,
		0x48, 0x00, 0x00, 0x00, 0x4C, 0x00, 0x00, 0x00,
		0x50, 0x00, 0x00, 0x00, 0x00, 0xC0, 0x01, 0x40,
		0xA0, 0x86, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
		0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
		0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
		0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
		0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
		0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
		0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
		0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
		0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
		0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
		0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	};

	/* Get working area for code */
	result = target_alloc_working_area(	target,
											sizeof(km1m7xx_write_code),
											&algorithm);
	if (result != ERROR_OK) {
		LOG_DEBUG("target_alloc_working_area() = %d\n", result);
		return(ERROR_TARGET_RESOURCE_NOT_AVAILABLE);
	}

	/* Transfer write program to RAM */
	result = target_write_buffer(	target,
									algorithm->address,
									sizeof(km1m7xx_write_code),
									km1m7xx_write_code);
	if (result != ERROR_OK) {
		LOG_DEBUG("target_write_buffer() = %d\n", result);
		target_free_working_area(target, algorithm);
		return(result);
	}

	/* Get working area for data */
	buffer_size	= 4 * 1024;
	result		= ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	while (result != ERROR_OK) {
		result = target_alloc_working_area_try(target, buffer_size, &source);
		if (result == ERROR_OK) {
			break;
		}
		buffer_size /= 2;
		if (buffer_size < 256) {
			LOG_DEBUG("target_alloc_working_area_try() = %d\n", result);
			target_free_working_area(target, algorithm);
			return(ERROR_TARGET_RESOURCE_NOT_AVAILABLE);
		}
	}

	/* Flash memory write enable */
	target_write_u32(bank->target, FEWEN,	(FEWEN_KEY_CODE | FEWEN_ENABLE));
	target_write_u32(bank->target, SPROSTR,	SPROSTR_ENABLE);
	target_write_u32(bank->target, SPROEND,	SPROEND_ENABLE);

	/**
	 *	Set parameter (Core Register)
	 *		Offset from last address of write program
	 *		SP		: <-  -0x14		: Stack Pointer
	 *		r9		: <-  -0x54		: .data Section
	 **/
	init_reg_param(&reg_params[0], "sp", 32, PARAM_OUT);
	init_reg_param(&reg_params[1], "r9", 32, PARAM_OUT);

	buf_set_u32(reg_params[0].value, 0, 32,
				(algorithm->address + sizeof(km1m7xx_write_code) - 0x14));
	buf_set_u32(reg_params[1].value, 0, 32,
				(algorithm->address + sizeof(km1m7xx_write_code) - 0x54));

	/**
	 *	Set parameter
	 *		Offset from last address of write program
	 *		(-0x10	:  -> Address      )
	 *		 -0x0C	:  -> BufferAddress
	 *		(-0x08	:  -> ByteCount    )
	 *		(-0x04	: <-  Result       )
	 **/
	mem_params32[1] = source->address;

	/* Program in units */
	remain_size		= count;
	write_address	= bank->base + offset;
	write_data		= (uint8_t*)buffer;
	write_size		= buffer_size;

	while (remain_size != 0) {
		if (remain_size < buffer_size) {
			write_size = remain_size;
		}

		LOG_INFO(	"Program at 0x%08x to 0x%08x\n", 
					write_address, (write_address + write_size -1));

		/**
		 *	Set parameter
		 *		Offset from last address of write program
		 *		 -0x10	:  -> Address
		 *		(-0x0C	:  -> BufferAddress )
		 *		 -0x08	:  -> ByteCount
		 *		 -0x04	: <-  Result
		 **/
		mem_params32[0] = write_address;
		mem_params32[2] = write_size;
		mem_params32[3] = 0;
		target_buffer_set_u32_array(	target,
										mem_params8,
										ARRAY_SIZE(mem_params32),
										mem_params32);
		result = target_write_buffer(
						target,
						algorithm->address + sizeof(km1m7xx_write_code) - 0x10,
						16,
						mem_params8);
		if (result != ERROR_OK) {
			LOG_DEBUG("target_write_buffer() = %d\n", result);
			break;
		}

		/* Set parameter (Write data) */
		result = target_write_buffer(	target,
										source->address,
										write_size,
										write_data);
		if (result != ERROR_OK) {
			LOG_DEBUG("target_write_buffer() = %d\n", result);
			break;
		}

		/* Run program */
		armv7m_info.common_magic	= ARMV7M_COMMON_MAGIC;
		armv7m_info.core_mode		= ARM_MODE_THREAD;
		result = target_run_algorithm(	target,
										0, NULL,
										ARRAY_SIZE(reg_params), reg_params,
										algorithm->address,
										0,
										1000,
										&armv7m_info);
		if (result != ERROR_OK) {
			LOG_DEBUG("target_run_algorithm() = %d\n", result);
			result = ERROR_FLASH_OPERATION_FAILED;
			break;
		}

		/* Get status */
		result = target_read_u32(
						target,
						algorithm->address + sizeof(km1m7xx_write_code) - 4,
						&status);
		if (result != ERROR_OK) {
			LOG_DEBUG("target_read_u32() = %d\n", result);
			break;
		}

		/* Next */
		remain_size		-= write_size;
		write_address	+= write_size;
		write_data		+= write_size;
	}

	/* Free allocated area */
	target_free_working_area(target, algorithm);
	target_free_working_area(target, source);
	destroy_reg_param(&reg_params[0]);
	destroy_reg_param(&reg_params[1]);

	return(result);
}

static int km1m7xx_probe(struct flash_bank *bank)
{
	int		result = ERROR_OK;

	struct km1m7xx_flash_bank	*km1m7xx_info;
	km1m7xx_info = bank->driver_priv;

	/**
	 *	Automatically determined when size = 0 is specified.
	 *		I-Flash : 512KB or 384KB or 256KB
	 *		D-Flash : 64KB
	 **/
	uint32_t	opt_reg00;

	if (bank->size == 0) {
		if (strstr(bank->name, "flash_d") != NULL) {
			bank->size = 0x00010000;
		} else if (strstr(bank->name, "flash_i") != NULL) {
			result = target_read_u32(bank->target, 0x4001C160, &opt_reg00);
			if (result != ERROR_OK) {
				/**
				 * Run km1m7xx_probe() again later
				 * by leaving km1m7xx_info->probed=0.
				 **/
				return(ERROR_OK);
			}
			bank->size = ((opt_reg00 & 0x00FF0000) >> 4);
		} else {
			bank->size = 0x00080000;
		}
	}

	bank->num_sectors	= bank->size / FLASH_SECTOR_SIZE;
	bank->sectors		= malloc(sizeof(struct flash_sector) * bank->num_sectors);

	int			cnt;
	uint32_t	offset = 0;
	for (cnt = 0; cnt < (int)(bank->num_sectors); cnt++) {
		bank->sectors[cnt].offset		= offset;
		bank->sectors[cnt].size			= FLASH_SECTOR_SIZE;
		bank->sectors[cnt].is_erased	= -1;
		bank->sectors[cnt].is_protected	= -1;
		offset += FLASH_SECTOR_SIZE;
	}

	km1m7xx_info->probed = 1;
	return(ERROR_OK);
}

static int km1m7xx_protect(struct flash_bank *bank, int set, unsigned int first, unsigned int last)
{
	LOG_INFO("protect function is unsupported\n");
	return(ERROR_FLASH_OPER_UNSUPPORTED);
}

static int km1m7xx_erase_check(struct flash_bank *bank)
{
	LOG_INFO("erase_check function is unsupported\n");
	return(ERROR_FLASH_OPER_UNSUPPORTED);
}

static int km1m7xx_protect_check(struct flash_bank *bank)
{
	LOG_INFO("protect_check function is unsupported\n");
	return(ERROR_OK);
}

static int km1m7xx_info(struct flash_bank *bank, char *buf, int buf_size)
{
	return(ERROR_OK);
}

static int km1m7xx_auto_probe(struct flash_bank *bank)
{
	struct km1m7xx_flash_bank *km1m7xx_info = bank->driver_priv;

	if (km1m7xx_info->probed) {
		return(ERROR_OK);
	}
	return(km1m7xx_probe(bank));
}

COMMAND_HANDLER(km1m7xx_handle_erase_all_sectors_command)
{
	struct flash_bank	*bank;
	int					result;

	/* Erase all sectors of each bank */
	for (bank = flash_bank_list(); bank != NULL; bank = bank->next) {

		/* Get bank information */
		get_flash_bank_by_name(bank->name, &bank);

		/* Erase all sectors */
		result = km1m7xx_erase(bank, 0, (bank->num_sectors - 1));
		if (result != ERROR_OK) {
			return(result);
		}
	}

	return(ERROR_OK);
}

COMMAND_HANDLER(km1m7xx_handle_check_image_area_command)
{

	struct flash_bank	*bank			= NULL;
	int					bank_num		= 0;
	int					result			= ERROR_OK;
	struct image		image;
	int					i;

	uint32_t			flash_sadrs[]	= {0x00000000, 0x00000000};
	uint32_t			flash_eadrs[]	= {0x00000000, 0x00000000};
	uint32_t			image_sadrs		= 0x00000000;
	uint32_t			image_eadrs		= 0x00000000;
	int					out_flag		= 0;

	if (CMD_ARGC != 1) {
		return(ERROR_COMMAND_SYNTAX_ERROR);
	}

	/* Get all sectors information of each bank */
	bank_num	= 0;
	bank		= flash_bank_list();
	while (bank != NULL) {

		if (bank_num > 2) {
			LOG_ERROR("2 bank over!!\n");
			return(ERROR_FLASH_OPER_UNSUPPORTED);
		}

		/* Get bank information */
		get_flash_bank_by_name(bank->name, &bank);

		flash_sadrs[bank_num]	= bank->base;
		flash_eadrs[bank_num]	= bank->base + bank->size - 1;

		bank_num++;
		bank = bank->next;
	}
	if (bank_num == 0) {
		LOG_ERROR("no bank!!\n");
		return(ERROR_FLASH_BANK_INVALID);
	}

	/* Check if the image contains data outside the flash memory area. */
	image.base_address_set	= 0;
	image.base_address		= 0;
	image.start_address_set	= 0;

	result = image_open(&image, CMD_ARGV[0], NULL);
	if (result != ERROR_OK) {
		return(result);
	}

	out_flag = 0;
	for (i = 0; i < image.num_sections; i++) {

		image_sadrs	= (uint32_t)image.sections[i].base_address;
		image_eadrs	= (uint32_t)image.sections[i].base_address + image.sections[i].size - 1;

		if (((image_sadrs < flash_sadrs[0]) || (image_eadrs > flash_eadrs[0])) &&
			((image_sadrs < flash_sadrs[1]) || (image_eadrs > flash_eadrs[1])) ) {
			LOG_ERROR(	"Section beyond flash memory area : "
						"0x%08x-0x%08x\n", image_sadrs, image_eadrs);
			out_flag = 1;
		}
	}
	if (out_flag == 1) {
		return(ERROR_FLASH_DST_OUT_OF_BANK);
	}

	return(ERROR_OK);
}

static const struct command_registration km1m7xx_subcommand_handlers[] = {
	{
		.name		= "erase_all_sectors",
		.handler	= km1m7xx_handle_erase_all_sectors_command,
		.mode		= COMMAND_EXEC,
		.usage		= "",
		.help		= "Erase all sectors",
	},
	{
		.name		= "check_image_area",
		.handler	= km1m7xx_handle_check_image_area_command,
		.mode		= COMMAND_ANY,
		.usage		= "",
		.help		= "Check if the image contains data outside the flash memory area.",
	},
	COMMAND_REGISTRATION_DONE
};

static const struct command_registration km1m7xx_command_handlers[] = {
	{
		.name		= "km1m7xx",
		.mode		= COMMAND_ANY,
		.help		= "km1m7xx command group",
		.usage		= "",
		.chain		= km1m7xx_subcommand_handlers,
	},
	COMMAND_REGISTRATION_DONE
};

struct flash_driver km1m7xx_flash = {
	.name					= "km1m7xx",
	.usage					= "",
	.commands				= km1m7xx_command_handlers,
	.flash_bank_command		= km1m7xx_flash_bank_command,
	.erase					= km1m7xx_erase,
	.protect				= km1m7xx_protect,
	.write					= km1m7xx_write,
	.read					= default_flash_read,
	.probe					= km1m7xx_probe,
	.auto_probe				= km1m7xx_auto_probe,
	.erase_check			= km1m7xx_erase_check,
	.protect_check			= km1m7xx_protect_check,
	.info					= km1m7xx_info,
	.free_driver_priv		= default_flash_free_driver_priv,
};
