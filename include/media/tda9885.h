/*
    tda9885.h

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#ifndef _TDA9886_H_
#define _TDA9885_H_

/**
 * struct tda9885m_platform_data - Platform data values and access functions.
 * @power_set: Power state access function, zero is off, non-zero is on.
 */
struct tda9885m_platform_data {
	int (*s_power) (struct v4l2_subdev *subdev, u32 on);
};

/**
 * direct power handling function
 */
extern int tda9885_power_on(int power);

#endif

