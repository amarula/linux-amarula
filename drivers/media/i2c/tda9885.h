#ifndef __LINUX_I2C_TDA9885_H
#define __LINUX_I2C_TDA9885_H

struct tda9885_platform_data {
	u8 switching_mode;
	u8 adjust_mode;
	u8 data_mode;
	int power;
};

#endif /* __LINUX_I2C_TDA9885_H */
