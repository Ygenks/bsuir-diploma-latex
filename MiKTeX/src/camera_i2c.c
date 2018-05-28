struct mt9t031 {
	struct subdev subdev;
	struct ctrl_handler hdl;
	struct {
		struct ctrl *autoexposure;
		struct ctrl *exposure;
	};
	struct rect rect;
	struct clk *clk;
	u16 xskip;
	u16 yskip;
	unsigned int total_h;
	unsigned short y_skip_top;
};

static struct mt9t031 *to_mt9t031(const struct i2c *client)
{
	return container_of(i2c_get_clientdata(client), struct mt9t031, subdev);
}

static int read(struct i2c *client, const u8 reg)
{
	return i2c_smbus_read_word_swapped(client, reg);
}

static int write(struct i2c *client, const u8 reg, const u16 data)
{
	return i2c_smbus_write_word_swapped(client, reg, data);
}

static int set(struct i2c *client, const u8 reg, const u16 data)
{
	int ret;

	ret = read(client, reg);
	if (ret < 0)
		return ret;
	return write(client, reg, ret | data);
}

static int clear(struct i2c *client, const u8 reg, const u16 data)
{
	int ret;

	ret = read(client, reg);
	if (ret < 0)
		return ret;
	return write(client, reg, ret & ~data);
}

static int mt9t031_set_params(struct i2c *client, struct rect *rect, u16 xskip,
			      u16 yskip)
{
	struct mt9t031 *mt9t031 = to_mt9t031(client);
	int ret;
	u16 xbin, ybin;
	const u16 hblank = MT9T031_HORIZONTAL_BLANK,
		  vblank = MT9T031_VERTICAL_BLANK;

	xbin = min(xskip, (u16)3);
	ybin = min(yskip, (u16)3);

	switch (xbin) {
	case 1:
		rect->left &= ~1;
		break;
	case 2:
		rect->left &= ~3;
		break;
	case 3:
		rect->left = rect->left > roundup(MT9T031_COLUMN_SKIP, 6)
				 ? (rect->left / 6) * 6
				 : roundup(MT9T031_COLUMN_SKIP, 6);
	}

	rect->top &= ~1;

	debug(&client->dev, "skip %u:%u, rect %ux%u@%u:%u\n", xskip, yskip,
	      rect->width, rect->height, rect->left, rect->top);

	ret = set(client, MT9T031_OUTPUT_CONTROL, 1);
	if (ret < 0)
		return ret;

	ret = write(client, MT9T031_HORIZONTAL_BLANKING, hblank);
	if (ret >= 0)
		ret = write(client, MT9T031_VERTICAL_BLANKING, vblank);

	if (yskip != mt9t031->yskip || xskip != mt9t031->xskip) {

		if (ret >= 0)
			ret = write(client, MT9T031_COLUMN_ADDRESS_MODE,
				    ((xbin - 1) << 4) | (xskip - 1));
		if (ret >= 0)
			ret = write(client, MT9T031_ROW_ADDRESS_MODE,
				    ((ybin - 1) << 4) | (yskip - 1));
	}
	debug(&client->dev, "new physical left %u, top %u\n", rect->left,
	      rect->top);

	if (ret >= 0)
		ret = write(client, MT9T031_COLUMN_START, rect->left);
	if (ret >= 0)
		ret = write(client, MT9T031_ROW_START, rect->top);
	if (ret >= 0)
		ret = write(client, MT9T031_WINDOW_WIDTH, rect->width - 1);
	if (ret >= 0)
		ret = write(client, MT9T031_WINDOW_HEIGHT,
			    rect->height + mt9t031->y_skip_top - 1);
	if (ret >= 0 && ctrl_g_ctrl(mt9t031->autoexposure) == EXPOSURE_AUTO) {
		mt9t031->total_h = rect->height + mt9t031->y_skip_top + vblank;

		ret = set_shutter(client, mt9t031->total_h);
	}

	if (ret >= 0)
		ret = clear(client, MT9T031_OUTPUT_CONTROL, 1);

	if (ret >= 0) {
		mt9t031->rect = *rect;
		mt9t031->xskip = xskip;
		mt9t031->yskip = yskip;
	}

	return ret < 0 ? ret : 0;
}

static int mt9t031_get_fmt(struct subdev *sd, struct subdev_pad_config *cfg,
			   struct subdev_format *format)
{
	struct mbus_framefmt *mf = &format->format;
	struct i2c *client = get_subdevdata(sd);
	struct mt9t031 *mt9t031 = to_mt9t031(client);

	if (format->pad)
		return -EINVAL;

	mf->width = mt9t031->rect.width / mt9t031->xskip;
	mf->height = mt9t031->rect.height / mt9t031->yskip;
	mf->code = MEDIA_BUS_FMT_SBGGR10_1X10;
	mf->colorspace = COLORSPACE_SRGB;
	mf->field = FIELD_NONE;

	return 0;
}

static int mt9t031_set_fmt(struct subdev *sd, struct subdev_pad_config *cfg,
			   struct subdev_format *format)
{
	struct mbus_framefmt *mf = &format->format;
	struct i2c *client = get_subdevdata(sd);
	struct mt9t031 *mt9t031 = to_mt9t031(client);
	u16 xskip, yskip;
	struct rect rect = mt9t031->rect;

	if (format->pad)
		return -EINVAL;

	mf->code = MEDIA_BUS_FMT_SBGGR10_1X10;
	mf->colorspace = COLORSPACE_SRGB;
	v4l_bound_align_image(&mf->width, MT9T031_MIN_WIDTH, MT9T031_MAX_WIDTH,
			      1, &mf->height, MT9T031_MIN_HEIGHT,
			      MT9T031_MAX_HEIGHT, 1, 0);

	if (format->which == SUBDEV_FORMAT_TRY) {
		cfg->try_fmt = *mf;
		return 0;
	}

	xskip = mt9t031_skip(&rect.width, mf->width, MT9T031_MAX_WIDTH);
	yskip = mt9t031_skip(&rect.height, mf->height, MT9T031_MAX_HEIGHT);

	mf->code = MEDIA_BUS_FMT_SBGGR10_1X10;
	mf->colorspace = COLORSPACE_SRGB;

	return mt9t031_set_params(client, &rect, xskip, yskip);
}

static int mt9t031_s_power(struct subdev *sd, int on)
{
	struct i2c *client = get_subdevdata(sd);
	struct soc_camera_subdev_desc *ssdd = soc_camera_i2c_to_desc(client);
	struct video_device *vdev = soc_camera_i2c_to_vdev(client);
	struct mt9t031 *mt9t031 = to_mt9t031(client);
	int ret;

	if (on) {
		ret = soc_camera_power_on(&client->dev, ssdd, mt9t031->clk);
		if (ret < 0)
			return ret;
		if (vdev)
			/* Not needed during probing, when vdev isn't available
			 * yet */
			vdev->dev.type = &mt9t031_dev_type;
	} else {
		if (vdev)
			vdev->dev.type = NULL;
		soc_camera_power_off(&client->dev, ssdd, mt9t031->clk);
	}

	return 0;
}

static int mt9t031_runtime_resume(struct device *dev)
{
	struct video_device *vdev = to_video_device(dev);
	struct subdev *sd = soc_camera_vdev_to_subdev(vdev);
	struct i2c *client = get_subdevdata(sd);
	struct mt9t031 *mt9t031 = to_mt9t031(client);

	int ret;
	u16 xbin, ybin;

	xbin = min(mt9t031->xskip, (u16)3);
	ybin = min(mt9t031->yskip, (u16)3);

	ret = write(client, MT9T031_COLUMN_ADDRESS_MODE,
		    ((xbin - 1) << 4) | (mt9t031->xskip - 1));
	if (ret < 0)
		return ret;

	ret = write(client, MT9T031_ROW_ADDRESS_MODE,
		    ((ybin - 1) << 4) | (mt9t031->yskip - 1));
	if (ret < 0)
		return ret;

	return 0;
}

static int mt9t031_s_ctrl(struct ctrl *ctrl)
{
	struct mt9t031 *mt9t031 =
	    container_of(ctrl->handler, struct mt9t031, hdl);
	struct subdev *sd = &mt9t031->subdev;
	struct i2c *client = get_subdevdata(sd);
	struct ctrl *exp = mt9t031->exposure;
	int data;

	switch (ctrl->id) {
	case CID_VFLIP:
		if (ctrl->val)
			data = set(client, MT9T031_READ_MODE_2, 0x8000);
		else
			data = clear(client, MT9T031_READ_MODE_2, 0x8000);
		if (data < 0)
			return -EIO;
		return 0;
	case CID_HFLIP:
		if (ctrl->val)
			data = set(client, MT9T031_READ_MODE_2, 0x4000);
		else
			data = clear(client, MT9T031_READ_MODE_2, 0x4000);
		if (data < 0)
			return -EIO;
		return 0;
	case CID_GAIN:

		if (ctrl->val <= ctrl->default_value) {

			unsigned long range =
			    ctrl->default_value - ctrl->minimum;
			data =
			    ((ctrl->val - (s32)ctrl->minimum) * 8 + range / 2) /
			    range;

			debug(&client->dev, "Setting gain %d\n", data);
			data = write(client, MT9T031_GLOBAL_GAIN, data);
			if (data < 0)
				return -EIO;
		} else {

			unsigned long range =
			    ctrl->maximum - ctrl->default_value - 1;

			unsigned long gain =
			    ((ctrl->val - (s32)ctrl->default_value - 1) * 1015 +
			     range / 2) /
				range +
			    9;

			if (gain <= 32)
				data = gain;
			else if (gain <= 64)
				data = ((gain - 32) * 16 + 16) / 32 + 80;
			else

				data = (((gain - 64 + 7) * 32) & 0xff00) | 0x60;

			debug(&client->dev, "Set gain from 0x%x to 0x%x\n",
			      read(client, MT9T031_GLOBAL_GAIN), data);
			data = write(client, MT9T031_GLOBAL_GAIN, data);
			if (data < 0)
				return -EIO;
		}
		return 0;

	case CID_EXPOSURE_AUTO:
		if (ctrl->val == EXPOSURE_MANUAL) {
			unsigned int range = exp->maximum - exp->minimum;
			unsigned int shutter =
			    ((exp->val - (s32)exp->minimum) * 1048 +
			     range / 2) /
				range +
			    1;
			u32 old;

			get_shutter(client, &old);
			debug(&client->dev, "Set shutter from %u to %u\n", old,
			      shutter);
			if (set_shutter(client, shutter) < 0)
				return -EIO;
		} else {
			const u16 vblank = MT9T031_VERTICAL_BLANK;
			mt9t031->total_h =
			    mt9t031->rect.height + mt9t031->y_skip_top + vblank;

			if (set_shutter(client, mt9t031->total_h) < 0)
				return -EIO;
		}
		return 0;
	default:
		return -EINVAL;
	}
	return 0;
}
