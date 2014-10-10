/*
 * Copyright (C) 2010 Trusted Logic S.A.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/i2c.h>
#include <linux/irq.h>
#include <linux/jiffies.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/miscdevice.h>
#include <linux/spinlock.h>
#include <linux/pn547.h>


#define MAX_BUFFER_SIZE	512

struct pn547_dev	{
	wait_queue_head_t	read_wq;
	struct mutex		read_mutex;
	struct i2c_client	*client;
	struct miscdevice	pn547_device;
	unsigned int		ven_gpio;
	unsigned int		firm_gpio;
	unsigned int		irq_gpio;
	bool			irq_enabled;
	spinlock_t		irq_enabled_lock;
};



static void pn547_disable_irq(struct pn547_dev *pn547_dev)
{
	unsigned long flags;

	spin_lock_irqsave(&pn547_dev->irq_enabled_lock, flags);
	if (pn547_dev->irq_enabled) {
		disable_irq_nosync(pn547_dev->client->irq);
		pn547_dev->irq_enabled = false;
	}
	spin_unlock_irqrestore(&pn547_dev->irq_enabled_lock, flags);
}

static irqreturn_t pn547_dev_irq_handler(int irq, void *dev_id)
{
	struct pn547_dev *pn547_dev = dev_id;

	pn547_disable_irq(pn547_dev);

	/* Wake up waiting readers */
	wake_up(&pn547_dev->read_wq);

	return IRQ_HANDLED;
}

static ssize_t pn547_dev_read(struct file *filp, char __user *buf,
		size_t count, loff_t *offset)
{
	struct pn547_dev *pn547_dev = filp->private_data;
	char tmp[MAX_BUFFER_SIZE];
	int ret;

	if (count > MAX_BUFFER_SIZE)
		count = MAX_BUFFER_SIZE;

	pr_info("%s : reading %zu bytes.\n", __func__, count);

	mutex_lock(&pn547_dev->read_mutex);

	if (!gpio_get_value(pn547_dev->irq_gpio)) {
		if (filp->f_flags & O_NONBLOCK) {
			ret = -EAGAIN;
			goto fail;
		}

		pn547_dev->irq_enabled = true;
		enable_irq(pn547_dev->client->irq);
		ret = wait_event_interruptible(pn547_dev->read_wq,
				gpio_get_value(pn547_dev->irq_gpio));

		pn547_disable_irq(pn547_dev);

		if (ret)
			goto fail;

	}

	/* Read data */
	ret = i2c_master_recv(pn547_dev->client, tmp, count);
	mutex_unlock(&pn547_dev->read_mutex);

	if (ret < 0) {
		pr_err("%s: i2c_master_recv returned %d\n", __func__, ret);
		return ret;
	}
	if (ret > count) {
		pr_err("%s: received too many bytes from i2c (%d)\n",
			__func__, ret);
		return -EIO;
	}
	if (copy_to_user(buf, tmp, ret)) {
		pr_warning("%s : failed to copy to user space\n", __func__);
		return -EFAULT;
	}
	return ret;

fail:
	mutex_unlock(&pn547_dev->read_mutex);
	return ret;
}

static ssize_t pn547_dev_write(struct file *filp, const char __user *buf,
		size_t count, loff_t *offset)
{
	struct pn547_dev  *pn547_dev;
	char tmp[MAX_BUFFER_SIZE];
	int ret;
	int i = 2 ;

	pn547_dev = filp->private_data;

	if (count > MAX_BUFFER_SIZE)
		count = MAX_BUFFER_SIZE;

	if (copy_from_user(tmp, buf, count)) {
		pr_err("%s : failed to copy from user space\n", __func__);
		return -EFAULT;
	}

	pr_err("%s : writing %zu bytes.\n", __func__, count);
	/* Write data */

	while(i--)
	{
		ret = i2c_master_send(pn547_dev->client, tmp, count);
		if( ret == count )
		{
			return ret;
		}

		msleep(20);
	};

	pr_err("%s : i2c_master_send returned %d\n", __func__, ret);
	return -EIO;
}

static int pn547_dev_open(struct inode *inode, struct file *filp)
{
	struct pn547_dev *pn547_dev = container_of(filp->private_data,
						struct pn547_dev,
						pn547_device);

	filp->private_data = pn547_dev;

	pr_info("%s : %d,%d\n", __func__, imajor(inode), iminor(inode));
	irq_set_irq_wake(pn547_dev->client->irq, 1);
	return 0;
}

static int pn547_dev_release(struct inode *inode, struct file *filp)
{
	struct pn547_dev *pn547_dev = filp->private_data;
	pr_info("close nfc\n");
	irq_set_irq_wake(pn547_dev->client->irq, 0);
	return 0;
}

static long pn547_dev_ioctl(struct file *filp,
			    unsigned int cmd, unsigned long arg)
{
	struct pn547_dev *pn547_dev = filp->private_data;

	switch (cmd) {
	case PN547_SET_PWR:
		if (arg == 2) {
			/* power on with firmware download (requires hw reset)
			 */
			pr_info("%s power on with firmware\n", __func__);
			gpio_set_value(pn547_dev->ven_gpio, 1);
			gpio_set_value(pn547_dev->firm_gpio, 1);
			msleep(20);
			gpio_set_value(pn547_dev->ven_gpio, 0);
			msleep(60);
			gpio_set_value(pn547_dev->ven_gpio, 1);
			msleep(20);
		} else if (arg == 1) {
			/* power on */
			pr_info("%s power on\n", __func__);
			gpio_set_value(pn547_dev->firm_gpio, 0);
			gpio_set_value(pn547_dev->ven_gpio, 1);
			msleep(20);
		} else  if (arg == 0) {
			/* power off */
			pr_info("%s power off\n", __func__);
			gpio_set_value(pn547_dev->firm_gpio, 0);
			gpio_set_value(pn547_dev->ven_gpio, 0);
			msleep(60);
		} else {
			pr_err("%s bad arg %lu\n", __func__, arg);
			return -EINVAL;
		}
		break;
	default:
		pr_err("%s bad ioctl %u\n", __func__, cmd);
		return -EINVAL;
	}

	return 0;
}

static const struct file_operations pn547_dev_fops = {
	.owner	= THIS_MODULE,
	.llseek	= no_llseek,
	.read	= pn547_dev_read,
	.write	= pn547_dev_write,
	.open	= pn547_dev_open,
	.unlocked_ioctl	= pn547_dev_ioctl,
	.release	= pn547_dev_release,
};




static int pn547_parse_dt(struct device *dev,
			struct pn547_i2c_platform_data *pdata)
{
	struct device_node *np = dev->of_node;

	pdata->irq_gpio = of_get_named_gpio_flags(np, "qcom,irq-gpio",
				0, &pdata->irq_gpio_flags);
	if (pdata->irq_gpio < 0)
		return pdata->irq_gpio;

	pdata->ven_gpio = of_get_named_gpio_flags(np, "qcom,enable-gpio",
				0, &pdata->ven_gpio_flags);
	if (pdata->ven_gpio < 0)
		return pdata->ven_gpio;

	pdata->firm_gpio = of_get_named_gpio_flags(np, "qcom,firm-gpio",
				0, &pdata->firm_gpio_flags);
	if (pdata->firm_gpio < 0)
		return pdata->firm_gpio;

	return 0;
}


static int pn547_probe(struct i2c_client *client,
		const struct i2c_device_id *id)
{
	int ret;
	struct pn547_dev *pn547_dev;
	struct pn547_i2c_platform_data  *platform_data;

	platform_data = kzalloc(sizeof(struct pn547_i2c_platform_data), GFP_KERNEL);


	pr_err("quark pn547_parse_dt enter \n");
	if (platform_data == NULL) {
		dev_err(&client->dev,
				"failed to allocate memory for module data\n");
		ret = -ENOMEM;
		goto err_exit;
	}
	client->dev.platform_data = platform_data;

	ret = pn547_parse_dt(&client->dev,platform_data );
	if (ret)
		goto err_dt;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		pr_err("%s : need I2C_FUNC_I2C\n", __func__);
		return  -ENODEV;
	}


	gpio_tlmm_config(GPIO_CFG(platform_data->irq_gpio, 0,
				  GPIO_CFG_INPUT, GPIO_CFG_PULL_DOWN,
				  GPIO_CFG_2MA), GPIO_CFG_ENABLE);
	gpio_tlmm_config(GPIO_CFG(platform_data->ven_gpio, 0,
				  GPIO_CFG_OUTPUT, GPIO_CFG_NO_PULL,
				  GPIO_CFG_2MA), GPIO_CFG_ENABLE);
	gpio_tlmm_config(GPIO_CFG(platform_data->firm_gpio, 0,
				  GPIO_CFG_OUTPUT, GPIO_CFG_NO_PULL,
				  GPIO_CFG_2MA), GPIO_CFG_ENABLE);

	ret = gpio_request(platform_data->irq_gpio, "nfc_int");
	if (ret)
	{
		printk("nfc_int");
		//return  -ENODEV;
	}

	ret = gpio_request(platform_data->ven_gpio, "nfc_ven");
	if (ret)
	{
		printk("nfc_ven");
		goto err_ven;
	}
	gpio_direction_output(platform_data->ven_gpio,0);

	ret = gpio_request(platform_data->firm_gpio, "nfc_firm");
	if (ret)
	{
		printk("nfc_firm");
		goto err_firm;
	}
	gpio_direction_output(platform_data->firm_gpio,0);



	pn547_dev = kzalloc(sizeof(*pn547_dev), GFP_KERNEL);
	if (pn547_dev == NULL) {
		dev_err(&client->dev,
				"failed to allocate memory for module data\n");
		ret = -ENOMEM;
		goto err_exit;
	}

	pn547_dev->irq_gpio = platform_data->irq_gpio;
	pn547_dev->ven_gpio  = platform_data->ven_gpio;
	pn547_dev->firm_gpio  = platform_data->firm_gpio;
	pn547_dev->client   = client;
	pn547_dev->client->addr = 0x2b;
	client->irq = gpio_to_irq(platform_data->irq_gpio);

	/* init mutex and queues */
	init_waitqueue_head(&pn547_dev->read_wq);
	mutex_init(&pn547_dev->read_mutex);
	spin_lock_init(&pn547_dev->irq_enabled_lock);

	pn547_dev->pn547_device.minor = MISC_DYNAMIC_MINOR;
	pn547_dev->pn547_device.name = "pn544";
	pn547_dev->pn547_device.fops = &pn547_dev_fops;

	ret = misc_register(&pn547_dev->pn547_device);
	if (ret) {
		pr_err("%s : misc_register failed\n", __FILE__);
		goto err_misc_register;
	}

	/* request irq.  the irq is set whenever the chip has data available
	 * for reading.  it is cleared when all data has been read.
	 */
	pr_info("%s : requesting IRQ %d\n", __func__, client->irq);
	pn547_dev->irq_enabled = true;
	ret = request_irq(client->irq, pn547_dev_irq_handler,
			  IRQF_TRIGGER_HIGH, client->name, pn547_dev);
	if (ret) {
		dev_err(&client->dev, "request_irq failed\n");
		goto err_request_irq_failed;
	}
	pn547_disable_irq(pn547_dev);
	i2c_set_clientdata(client, pn547_dev);
	
	pr_err("quark pn547_parse_dt exit \n");

	return 0;

err_request_irq_failed:
	misc_deregister(&pn547_dev->pn547_device);
	printk(" nfc err_request_irq_failed \n");
err_misc_register:
	mutex_destroy(&pn547_dev->read_mutex);
	kfree(pn547_dev);
	printk("nfc err_misc_register \n");
err_exit:
	dev_err(&client->dev, "pn547_parse_dt failed %d\n",ret);
err_dt:
	printk("nfc platform_data->firm_gpio %d \n", platform_data->firm_gpio);
	gpio_free(platform_data->firm_gpio);
err_firm:
	printk("nfc platform_data->ven_gpio %d\n",platform_data->ven_gpio);
	gpio_free(platform_data->ven_gpio);
err_ven:
	printk("nfc platform_data->irq_gpio  %d\n",platform_data->irq_gpio);
	gpio_free(platform_data->irq_gpio);
	return ret;
}

static int pn547_remove(struct i2c_client *client)
{
	struct pn547_dev *pn547_dev;

	pn547_dev = i2c_get_clientdata(client);
	free_irq(client->irq, pn547_dev);
	misc_deregister(&pn547_dev->pn547_device);
	mutex_destroy(&pn547_dev->read_mutex);

	gpio_free(pn547_dev->irq_gpio);
	gpio_free(pn547_dev->ven_gpio);
	gpio_free(pn547_dev->firm_gpio);
	kfree(client->dev.platform_data);
	kfree(pn547_dev);

	return 0;
}

void pn547_shutdown(struct i2c_client *client )
{
	struct pn547_dev *pn547_dev;

	pn547_dev = i2c_get_clientdata(client);

	gpio_set_value(pn547_dev->ven_gpio, 1);
	msleep(20);
	gpio_set_value(pn547_dev->ven_gpio, 0);
	msleep(60);
	gpio_set_value(pn547_dev->ven_gpio, 1);

}


static const struct of_device_id msm_nfc_dt_match[] = {
	{.compatible = "nfc-pn547"},
	{}
};


static const struct i2c_device_id pn547_id[] = {
	{ "nfc-pn547", 0 },
	{ }
};

static struct i2c_driver pn547_driver = {
	.id_table	= pn547_id,
	.probe		= pn547_probe,
	.remove		= pn547_remove,
	.shutdown	= pn547_shutdown,
	.driver		= {
		.owner	= THIS_MODULE,
		.name	= PN547_DRIVER_NAME,
		.of_match_table = msm_nfc_dt_match
	},
};

/*
 * module load/unload record keeping
 */

static int __init pn547_dev_init(void)
{
	pr_info("Loading pn547 driver\n");
	return i2c_add_driver(&pn547_driver);
}
module_init(pn547_dev_init);

static void __exit pn547_dev_exit(void)
{
	pr_info("Unloading pn547 driver\n");
	i2c_del_driver(&pn547_driver);
}
module_exit(pn547_dev_exit);

MODULE_AUTHOR("Sylvain Fonteneau");
MODULE_DESCRIPTION("NFC PN547 driver");
MODULE_LICENSE("GPL");
