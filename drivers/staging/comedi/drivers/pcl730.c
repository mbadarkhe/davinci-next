/*
 * comedi/drivers/pcl730.c
 * Driver for Advantech PCL-730 and clones
 * José Luis Sánchez
 */

/*
 * Driver: pcl730
 * Description: Advantech PCL-730 (& compatibles)
 * Devices: (Advantech) PCL-730 [pcl730]
 *	    (ICP) ISO-730 [iso730]
 *	    (Adlink) ACL-7130 [acl7130]
 * Author: José Luis Sánchez (jsanchezv@teleline.es)
 * Status: untested
 *
 * Configuration options:
 *   [0] - I/O port base
 *
 * Interrupts are not supported.
 * The ACL-7130 card has an 8254 timer/counter not supported by this driver.
 */

#include "../comedidev.h"

#include <linux/ioport.h>

#define PCL730_IDIO_LO	0	/* Isolated Digital I/O low byte (ID0-ID7) */
#define PCL730_IDIO_HI	1	/* Isolated Digital I/O high byte (ID8-ID15) */
#define PCL730_DIO_LO	2	/* TTL Digital I/O low byte (D0-D7) */
#define PCL730_DIO_HI	3	/* TTL Digital I/O high byte (D8-D15) */

struct pcl730_board {
	const char *name;	/*  board name */
	unsigned int io_range;	/*  len of I/O space */
};

static const struct pcl730_board pcl730_boards[] = {
	{
		.name		= "pcl730",
		.io_range	= 0x04,
	}, {
		.name		= "iso730",
		.io_range	= 0x04,
	}, {
		.name		= "acl7130",
		.io_range	= 0x08,
	},
};

static int pcl730_do_insn_bits(struct comedi_device *dev,
			       struct comedi_subdevice *s,
			       struct comedi_insn *insn,
			       unsigned int *data)
{
	unsigned long reg = (unsigned long)s->private;
	unsigned int mask = data[0];
	unsigned int bits = data[1];

	if (mask) {
		s->state &= ~mask;
		s->state |= (bits & mask);

		if (mask & 0x00ff)
			outb(s->state & 0xff, dev->iobase + reg);
		if (mask & 0xff00)
			outb((s->state >> 8) & 0xff, dev->iobase + reg + 1);
	}

	data[1] = s->state;

	return insn->n;
}

static int pcl730_di_insn_bits(struct comedi_device *dev,
			       struct comedi_subdevice *s,
			       struct comedi_insn *insn,
			       unsigned int *data)
{
	unsigned long reg = (unsigned long)s->private;

	data[1] = inb(dev->iobase + reg) | (inb(dev->iobase + reg + 1) << 8);

	return insn->n;
}

static int pcl730_attach(struct comedi_device *dev,
			 struct comedi_devconfig *it)
{
	const struct pcl730_board *board = comedi_board(dev);
	struct comedi_subdevice *s;
	int ret;

	ret = comedi_request_region(dev, it->options[0], board->io_range);
	if (ret)
		return ret;

	ret = comedi_alloc_subdevices(dev, 4);
	if (ret)
		return ret;

	/* Isolated Digital Outputs */
	s = &dev->subdevices[0];
	s->type		= COMEDI_SUBD_DO;
	s->subdev_flags	= SDF_WRITABLE;
	s->n_chan	= 16;
	s->maxdata	= 1;
	s->range_table	= &range_digital;
	s->insn_bits	= pcl730_do_insn_bits;
	s->private	= (void *)PCL730_IDIO_LO;

	/* Isolated Digital Inputs */
	s = &dev->subdevices[1];
	s->type		= COMEDI_SUBD_DI;
	s->subdev_flags	= SDF_READABLE;
	s->n_chan	= 16;
	s->maxdata	= 1;
	s->range_table	= &range_digital;
	s->insn_bits	= pcl730_di_insn_bits;
	s->private	= (void *)PCL730_IDIO_LO;

	/* TTL Digital Outputs */
	s = &dev->subdevices[2];
	s->type		= COMEDI_SUBD_DO;
	s->subdev_flags	= SDF_WRITABLE;
	s->n_chan	= 16;
	s->maxdata	= 1;
	s->range_table	= &range_digital;
	s->insn_bits	= pcl730_do_insn_bits;
	s->private	= (void *)PCL730_DIO_LO;

	/* TTL Digital Inputs */
	s = &dev->subdevices[3];
	s->type		= COMEDI_SUBD_DI;
	s->subdev_flags	= SDF_READABLE;
	s->n_chan	= 16;
	s->maxdata	= 1;
	s->range_table	= &range_digital;
	s->insn_bits	= pcl730_di_insn_bits;
	s->private	= (void *)PCL730_DIO_LO;

	return 0;
}

static struct comedi_driver pcl730_driver = {
	.driver_name	= "pcl730",
	.module		= THIS_MODULE,
	.attach		= pcl730_attach,
	.detach		= comedi_legacy_detach,
	.board_name	= &pcl730_boards[0].name,
	.num_names	= ARRAY_SIZE(pcl730_boards),
	.offset		= sizeof(struct pcl730_board),
};
module_comedi_driver(pcl730_driver);

MODULE_AUTHOR("Comedi http://www.comedi.org");
MODULE_DESCRIPTION("Comedi low-level driver");
MODULE_LICENSE("GPL");
