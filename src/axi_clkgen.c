#include <clk_hw.h>

#define CLKGEN_V2_REG_RESET 0x40
#define CLKGEN_V2_REG_CLKSEL 0x44
#define CLKGEN_V2_REG_DRP_CNTRL 0x70
#define CLKGEN_V2_REG_DRP_STATUS 0x74

#define CLKGEN_V2_RESET_MMCM_ENABLE BIT(1)
#define CLKGEN_V2_RESET_ENABLE BIT(0)

#define CLKGEN_V2_DRP_CNTRL_SEL BIT(29)
#define CLKGEN_V2_DRP_CNTRL_READ BIT(28)

#define CLKGEN_V2_DRP_STATUS_BUSY BIT(16)

#define MMCM_REG_CLKOUT0_1 0x08
#define MMCM_REG_CLKOUT0_2 0x09
#define MMCM_REG_CLK_FB1 0x14
#define MMCM_REG_CLK_FB2 0x15
#define MMCM_REG_CLK_DIV 0x16
#define MMCM_REG_LOCK1 0x18
#define MMCM_REG_LOCK2 0x19
#define MMCM_REG_LOCK3 0x1a
#define MMCM_REG_FILTER1 0x4e
#define MMCM_REG_FILTER2 0x4f

struct clkgen {
	void __iomem *base;
	struct clk_hw clk_hw;
};

static uint32_t lookup_filter(unsigned int m)
{
	switch (m) {
	case 0:
		return 0x01001990;
	case 1:
		return 0x01001190;
	case 2:
		return 0x01009890;
	case 3:
		return 0x01001890;
	case 4:
		return 0x01008890;
	case 5 ... 8:
		return 0x01009090;
	case 9 ... 11:
		return 0x01000890;
	case 12:
		return 0x08009090;
	case 13 ... 22:
		return 0x01001090;
	case 23 ... 36:
		return 0x01008090;
	case 37 ... 46:
		return 0x08001090;
	default:
		return 0x08008090;
	}
}

static const uint32_t lock_table[] = {
    0x060603e8, 0x060603e8, 0x080803e8, 0x0b0b03e8, 0x0e0e03e8, 0x111103e8,
    0x131303e8, 0x161603e8, 0x191903e8, 0x1c1c03e8, 0x1f1f0384, 0x1f1f0339,
    0x1f1f02ee, 0x1f1f02bc, 0x1f1f028a, 0x1f1f0271, 0x1f1f023f, 0x1f1f0226,
    0x1f1f020d, 0x1f1f01f4, 0x1f1f01db, 0x1f1f01c2, 0x1f1f01a9, 0x1f1f0190,
    0x1f1f0190, 0x1f1f0177, 0x1f1f015e, 0x1f1f015e, 0x1f1f0145, 0x1f1f0145,
    0x1f1f012c, 0x1f1f012c, 0x1f1f012c, 0x1f1f0113, 0x1f1f0113, 0x1f1f0113,
};

static uint32_t lookup_lock(unsigned int m)
{
	if (m < ARRAY_SIZE(lock_table))
		return lock_table[m];
	return 0x1f1f00fa;
}

static const unsigned int fpfd_min = 10000;
static const unsigned int fpfd_max = 300000;
static const unsigned int fvco_min = 600000;
static const unsigned int fvco_max = 1200000;

static void calc_params(unsigned long fin, unsigned long fout,
			       unsigned int *best_d, unsigned int *best_m,
			       unsigned int *best_dout)
{
	unsigned long d, d_min, d_max, _d_min, _d_max;
	unsigned long m, m_min, m_max;
	unsigned long f, dout, best_f, fvco;

	fin /= 1000;
	fout /= 1000;

	best_f = ULONG_MAX;
	*best_d = 0;
	*best_m = 0;
	*best_dout = 0;

	d_min = max_t(unsigned long, DIV_ROUND_UP(fin, fpfd_max), 1);
	d_max = min_t(unsigned long, fin / fpfd_min, 80);

	m_min = max_t(unsigned long, DIV_ROUND_UP(fvco_min, fin) * d_min, 1);
	m_max = min_t(unsigned long, fvco_max *d_max / fin, 64);

	for (m = m_min; m <= m_max; m++) {
		_d_min = max(d_min, DIV_ROUND_UP(fin * m, fvco_max));
		_d_max = min(d_max, fin * m / fvco_min);

		for (d = _d_min; d <= _d_max; d++) {
			fvco = fin * m / d;

			dout = DIV_ROUND_CLOSEST(fvco, fout);
			dout = clamp_t(unsigned long, dout, 1, 128);
			f = fvco / dout;
			if (abs(f - fout) < abs(best_f - fout)) {
				best_f = f;
				*best_d = d;
				*best_m = m;
				*best_dout = dout;
				if (best_f == fout)
					return;
			}
		}
	}
}

static void calc_clk_params(unsigned int divider, unsigned int *low,
				   unsigned int *high, unsigned int *edge,
				   unsigned int *nocount)
{
	if (divider == 1)
		*nocount = 1;
	else
		*nocount = 0;

	*high = divider / 2;
	*edge = divider % 2;
	*low = divider - *high;
}

static void read(unsigned long register, unsigned long *value)
{
	*value = Xil_In32(CF_CLKGEN_BASEADDR + register);
}

static void write(unsigned long register, unsigned long value)
{
	Xil_Out32(CF_CLKGEN_BASEADDR + register, value);
}

static int mmcm_read(unsigned int reg, unsigned int *val)
{
	unsigned int reg_val;
	int ret;

	ret = wait_non_busy(axi_clkgen);
	if (ret < 0) {
		return ret;
	}

	reg_val = CLKGEN_V2_DRP_CNTRL_SEL | CLKGEN_V2_DRP_CNTRL_READ;
	reg_val |= (reg << 16);

	write(CLKGEN_V2_REG_DRP_CNTRL, reg_val);

	ret = wait_non_busy(axi_clkgen);
	if (ret < 0) {
		return ret;
	}

	*val = ret;

	return 0;
}

static int mmcm_write(unsigned int reg, unsigned int val, unsigned int mask)
{
	unsigned int reg_val = 0;
	int ret;

	ret = wait_non_busy(axi_clkgen);
	if (ret < 0) {
		return ret;
	}

	if (mask != 0xffff) {
		mmcm_read(reg, &reg_val);
		reg_val &= ~mask;
	}

	reg_val |= CLKGEN_V2_DRP_CNTRL_SEL | (reg << 16) | (val & mask);

	write(CLKGEN_V2_REG_DRP_CNTRL, reg_val);

	return 0;
}
static int wait_non_busy(struct axi_clkgen *axi_clkgen)
{
	unsigned int timeout = 10000;
	unsigned int val;

	do {
		read(CLKGEN_V2_REG_DRP_STATUS, &val);
	} while ((val & CLKGEN_V2_DRP_STATUS_BUSY) && --timeout);

	if (val & CLKGEN_V2_DRP_STATUS_BUSY)
		return -EIO;

	return val & 0xffff;
}

static int set_rate(struct clk_hw *clk_hw, unsigned long rate,
		    unsigned long parent_rate)
{
	*axi_clkgen = clk_hw_to_axi_clkgen(clk_hw);
	unsigned int d, m, dout;
	unsigned int nocount;
	unsigned int high;
	unsigned int edge;
	unsigned int low;
	uint32_t filter;
	uint32_t lock;

	if (parent_rate == 0 || rate == 0)
		return -EINVAL;

	calc_params(parent_rate, rate, &d, &m, &dout);

	if (d == 0 || dout == 0 || m == 0)
		return -EINVAL;

	filter = lookup_filter(m - 1);
	lock = lookup_lock(m - 1);

	calc_clk_params(dout, &low, &high, &edge, &nocount);
	mmcm_write(MMCM_REG_CLKOUT0_1, (high << 6) | low, 0xefff);
	mmcm_write(MMCM_REG_CLKOUT0_2, (edge << 7) | (nocount << 6), 0x03ff);

	calc_clk_params(d, &low, &high, &edge, &nocount);
	mmcm_write(MMCM_REG_CLK_DIV,
		   (edge << 13) | (nocount << 12) | (high << 6) | low, 0x3fff);

	calc_clk_params(m, &low, &high, &edge, &nocount);
	mmcm_write(MMCM_REG_CLK_FB1, (high << 6) | low, 0xefff);
	mmcm_write(MMCM_REG_CLK_FB2, (edge << 7) | (nocount << 6), 0x03ff);

	mmcm_write(MMCM_REG_LOCK1, lock & 0x3ff, 0x3ff);
	mmcm_write(MMCM_REG_LOCK2, (((lock >> 16) & 0x1f) << 10) | 0x1, 0x7fff);
	mmcm_write(MMCM_REG_LOCK3, (((lock >> 24) & 0x1f) << 10) | 0x3e9,
		   0x7fff);
	mmcm_write(MMCM_REG_FILTER1, filter >> 16, 0x9900);
	mmcm_write(MMCM_REG_FILTER2, filter, 0x9900);

	return 0;
}

static void mmcm_enable(struct clkgen *clkgen, bool enable)
{
	unsigned int val = CLKGEN_V2_RESET_ENABLE;

	if (enable)
		val |= CLKGEN_V2_RESET_MMCM_ENABLE;

	write(clkgen, CLKGEN_V2_REG_RESET, val);
}

static struct clkgen *clk_hw_to_clkgen(struct clk_hw *clk_hw)
{
	return container_of(clk_hw, struct clkgen, clk_hw);
}

static int set_rate(struct clk_hw *clk_hw, unsigned long rate,
			   unsigned long parent_rate)
{
	struct clkgen *clkgen = clk_hw_to_clkgen(clk_hw);
	unsigned int d, m, dout;
	unsigned int nocount;
	unsigned int high;
	unsigned int edge;
	unsigned int low;
	uint32_t filter;
	uint32_t lock;

	if (parent_rate == 0 || rate == 0)
		return -EINVAL;

	calc_params(parent_rate, rate, &d, &m, &dout);

	if (d == 0 || dout == 0 || m == 0)
		return -EINVAL;

	filter = lookup_filter(m - 1);
	lock = lookup_lock(m - 1);

	calc_clk_params(dout, &low, &high, &edge, &nocount);
	mmcm_write(clkgen, MMCM_REG_CLKOUT0_1, (high << 6) | low,
			  0xefff);
	mmcm_write(clkgen, MMCM_REG_CLKOUT0_2,
			  (edge << 7) | (nocount << 6), 0x03ff);

	calc_clk_params(d, &low, &high, &edge, &nocount);
	mmcm_write(clkgen, MMCM_REG_CLK_DIV,
			  (edge << 13) | (nocount << 12) | (high << 6) | low,
			  0x3fff);

	calc_clk_params(m, &low, &high, &edge, &nocount);
	mmcm_write(clkgen, MMCM_REG_CLK_FB1, (high << 6) | low, 0xefff);
	mmcm_write(clkgen, MMCM_REG_CLK_FB2,
			  (edge << 7) | (nocount << 6), 0x03ff);

	mmcm_write(clkgen, MMCM_REG_LOCK1, lock & 0x3ff, 0x3ff);
	mmcm_write(clkgen, MMCM_REG_LOCK2,
			  (((lock >> 16) & 0x1f) << 10) | 0x1, 0x7fff);
	mmcm_write(clkgen, MMCM_REG_LOCK3,
			  (((lock >> 24) & 0x1f) << 10) | 0x3e9, 0x7fff);
	mmcm_write(clkgen, MMCM_REG_FILTER1, filter >> 16, 0x9900);
	mmcm_write(clkgen, MMCM_REG_FILTER2, filter, 0x9900);

	return 0;
}

static long round_rate(struct clk_hw *hw, unsigned long rate,
			      unsigned long *parent_rate)
{
	unsigned int d, m, dout;

	calc_params(*parent_rate, rate, &d, &m, &dout);

	if (d == 0 || dout == 0 || m == 0)
		return -EINVAL;

	return *parent_rate / d * m / dout;
}

static unsigned long recalc_rate(struct clk_hw *clk_hw,
					unsigned long parent_rate)
{
	struct clkgen *clkgen = clk_hw_to_clkgen(clk_hw);
	unsigned int d, m, dout;
	unsigned int reg;
	unsigned long long tmp;

	mmcm_read(clkgen, MMCM_REG_CLKOUT0_1, &reg);
	dout = (reg & 0x3f) + ((reg >> 6) & 0x3f);
	mmcm_read(clkgen, MMCM_REG_CLK_DIV, &reg);
	d = (reg & 0x3f) + ((reg >> 6) & 0x3f);
	mmcm_read(clkgen, MMCM_REG_CLK_FB1, &reg);
	m = (reg & 0x3f) + ((reg >> 6) & 0x3f);

	if (d == 0 || dout == 0)
		return 0;

	tmp = (unsigned long long)(parent_rate / d) * m;
	do_div(tmp, dout);

	return min_t(unsigned long long, tmp, ULONG_MAX);
}

static int enable(struct clk_hw *clk_hw)
{
	struct clkgen *clkgen = clk_hw_to_clkgen(clk_hw);

	mmcm_enable(clkgen, true);

	return 0;
}

static void disable(struct clk_hw *clk_hw)
{
	struct clkgen *clkgen = clk_hw_to_clkgen(clk_hw);

	mmcm_enable(clkgen, false);
}

static int set_parent(struct clk_hw *clk_hw, u8 index)
{
	struct clkgen *clkgen = clk_hw_to_clkgen(clk_hw);

	write(clkgen, CLKGEN_V2_REG_CLKSEL, index);

	return 0;
}

static u8 get_parent(struct clk_hw *clk_hw)
{
	struct clkgen *clkgen = clk_hw_to_clkgen(clk_hw);
	unsigned int parent;

	read(clkgen, CLKGEN_V2_REG_CLKSEL, &parent);

	return parent;
}

static const struct clk_ops ops = {
    .recalc_rate = recalc_rate,
    .round_rate = round_rate,
    .set_rate = set_rate,
    .enable = enable,
    .disable = disable,
    .set_parent = set_parent,
    .get_parent = get_parent,
};

static int probe(struct platform_device *pdev)
{
	const struct of_device_id *id;
	struct clkgen *clkgen;
	struct clk_init_data init;
	const char *parent_names[2];
	const char *clk_name;
	struct resource *mem;
	unsigned int i;
	int ret;

	if (!pdev->dev.of_node)
		return -ENODEV;

	id = of_match_node(ids, pdev->dev.of_node);
	if (!id)
		return -ENODEV;

	clkgen = xil_malloc(&pdev->dev, sizeof(*clkgen));
	if (!clkgen)
		return -ENOMEM;

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	clkgen->base = devm_ioremap_resource(&pdev->dev, mem);
	if (IS_ERR(clkgen->base))
		return PTR_ERR(clkgen->base);

	init.num_parents = of_clk_get_parent_count(pdev->dev.of_node);
	if (init.num_parents < 1 || init.num_parents > 2)
		return -EINVAL;

	for (i = 0; i < init.num_parents; i++) {
		parent_names[i] = of_clk_get_parent_name(pdev->dev.of_node, i);
		if (!parent_names[i])
			return -EINVAL;
	}

	clk_name = pdev->dev.of_node->name;
	of_property_read_string(pdev->dev.of_node, "clock-output-names",
				&clk_name);

	init.name = clk_name;
	init.ops = &ops;
	init.flags = CLK_SET_RATE_GATE | CLK_SET_PARENT_GATE;
	init.parent_names = parent_names;

	mmcm_enable(clkgen, false);

	clkgen->clk_hw.init = &init;
	ret = devm_clk_hw_register(&pdev->dev, &clkgen->clk_hw);
	if (ret)
		return ret;

	return of_clk_add_hw_provider(pdev->dev.of_node, of_clk_hw_simple_get,
				      &clkgen->clk_hw);
}

static int remove(struct platform_device *pdev)
{
	of_clk_del_provider(pdev->dev.of_node);

	return 0;
}

static struct platform_driver driver = {
    .driver =
	{
	    .name = "adi-axi-clkgen",
	    .of_match_table = ids,
	},
    .probe = probe,
    .remove = remove,
};
