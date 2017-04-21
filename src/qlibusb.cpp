#include "qlibusb.h"

QUsbDevice::QUsbDevice(QBaseUsbDevice *parent) :
    QBaseUsbDevice(parent)
{
    mDevHandle = NULL;
    int r = libusb_init(&mCtx); //initialize the library for the session we just declared
    if(r < 0) {
        qCritical() << "LibUsb Init Error " << r; //there was an error
    }
}

QUsbDevice::~QUsbDevice()
{
    this->close();
    libusb_exit(mCtx);
}

QtUsb::FilterList QUsbDevice::getAvailableDevices()
{
    QtUsb::FilterList list;

    ssize_t cnt; // holding number of devices in list

    libusb_device **devs;
    libusb_context *ctx;

    libusb_init(&ctx);
    cnt = libusb_get_device_list(ctx, &devs); // get the list of devices
    if(cnt < 0) {
        qCritical() << "Get Device List Error";
        libusb_free_device_list(devs, 1);
        return list;
    }

    for (int i=0; i< cnt; i++)
    {
        libusb_device* dev = devs[i];
        libusb_device_descriptor desc;

        if (libusb_get_device_descriptor(dev, &desc) == 0)
        {
            QtUsb::DeviceFilter filter;
            filter.pid = desc.idProduct;
            filter.vid = desc.idVendor;
            filter.guid = "";

            list.append(filter);
        }
    }

    libusb_free_device_list(devs, 1);
    libusb_exit(ctx);
    return list;
}

qint32 QUsbDevice::open()
{
    UsbPrintFuncName();
    if (mConnected)
        return -1;

    int r; // for return values
    ssize_t cnt; // holding number of devices in list

    cnt = libusb_get_device_list(mCtx, &mDevs); // get the list of devices
    if(cnt < 0) {
        qCritical() << "Get Device List Error";
        return -1;
    }

    mDevHandle = libusb_open_device_with_vid_pid(mCtx, mFilter.vid, mFilter.pid); // Open device
    if(mDevHandle == NULL) {
        qWarning() << "Cannot open device";
        return -2;
    }

    if (mDebug) qDebug() << "Device Opened";
    libusb_free_device_list(mDevs, 1); // free the list, unref the devices in it

    if(libusb_kernel_driver_active(mDevHandle, mConfig.interface) == 1) { // find out if kernel driver is attached
        if (mDebug) qDebug() << "Kernel Driver Active";
        if(libusb_detach_kernel_driver(mDevHandle, mConfig.interface) == 0) // detach it
            if (mDebug) qDebug() << "Kernel Driver Detached!";
    }

    int conf;
    libusb_get_configuration(mDevHandle, &conf);

    if (conf != mConfig.config) {
        if (mDebug) qDebug() << "Configuration needs to be changed";
        r = libusb_set_configuration(mDevHandle, mConfig.config);
        if(r != 0) {
            qWarning() << "Cannot Set Configuration";
            this->printUsbError(r);
            return -3;
        }
    }
    r = libusb_claim_interface(mDevHandle, mConfig.interface);
    if(r != 0) {
        qWarning() << "Cannot Claim Interface";
        this->printUsbError(r);
        return -4;
    }

    libusb_device* dev = libusb_get_device(mDevHandle);

    switch  (libusb_get_device_speed(dev))
    {
        case LIBUSB_SPEED_LOW:
            this->mSpd = QtUsb::lowSpeed;
            break;
        case LIBUSB_SPEED_FULL:
            this->mSpd = QtUsb::fullSpeed;
            break;
        case LIBUSB_SPEED_HIGH:
            this->mSpd = QtUsb::highSpeed;
            break;
        case LIBUSB_SPEED_SUPER:
            this->mSpd = QtUsb::superSpeed;
            break;
        default:
            this->mSpd = QtUsb::unknownSpeed;
            break;
    }

    mConnected = true;

    return 0;
}

void QUsbDevice::close()
{
    UsbPrintFuncName();
    if (mDevHandle && mConnected) {
        // stop any further write attempts whilst we close down
        qDebug("Closing USB connection...");

        QBaseUsbDevice::close();

        libusb_release_interface(mDevHandle, 0); //release the claimed interface
        libusb_close(mDevHandle); //close the device we opened
    }

    mConnected = false;
}

void QUsbDevice::setDebug(bool enable)
{
    QBaseUsbDevice::setDebug(enable);
    if (enable)
        libusb_set_debug(mCtx, 3);
    else
        libusb_set_debug(mCtx, 0);
}

void QUsbDevice::printUsbError(int error_code)
{
    qWarning("libusb Error: %s", libusb_strerror((enum libusb_error)error_code));
}

void QUsbDevice::flush()
{
    QByteArray buf;
    qint32 read_bytes;

    buf.resize(4096);
    libusb_bulk_transfer(mDevHandle, mConfig.readEp, (uchar*)(buf.data()), 4096, &read_bytes, 25);
}

qint32 QUsbDevice::read(QByteArray* buf, quint32 maxSize)
{
    UsbPrintFuncName();
    Q_CHECK_PTR(buf);
    qint32 rc, read_bytes;
    qint32 read_total;
    quint32 bsize = 1024;
    QElapsedTimer timer;

    // check it isn't closed already
    if (!mDevHandle || !mConnected) return -1;

    if (maxSize == 0)
        return 0;

    read_total = 0;
    read_bytes = 0;

    buf->resize(bsize);

    timer.start();
    while (timer.elapsed() < mTimeout && maxSize-read_total > 0) {
        rc = libusb_bulk_transfer(mDevHandle, mConfig.readEp, (uchar*)(buf->data()+read_total), bsize, &read_bytes, 1);
        if (rc == LIBUSB_ERROR_PIPE) {
            libusb_clear_halt(mDevHandle, mConfig.readEp);
        }
        if (rc == LIBUSB_ERROR_TIMEOUT)
            rc = 0;
        read_total += read_bytes;
        if (rc != 0) break;
    }
    // we resize the buffer.
    buf->resize(read_total);


    if (rc != LIBUSB_SUCCESS) {
        printUsbError(rc);
    }

    QString datastr, s;

    for (qint32 i = 0; i < read_total; i++) {
        if (mDebug) datastr.append(s.sprintf("%02X:", (uchar)buf->at(i)));
    }
    if (mDebug) {
        datastr.remove(datastr.size()-1, 1); //remove last colon
        qDebug("%d out of %d Received: %s", read_total, maxSize, datastr.toStdString().c_str());
    }

    if (rc != 0)
    {
        if (rc == -110)
        {
            qWarning("libusb_bulk_transfer Timeout");
        }
        else {
            qWarning("libusb_bulk_transfer Error reading: %d", rc);
            this->printUsbError(rc);
            return -1;
        }
    }

    return read_total;
}

qint32 QUsbDevice::write(const QByteArray* buf, quint32 maxSize)
{
    UsbPrintFuncName();
    Q_CHECK_PTR(buf);
    qint32 rc, sent_tmp;
    qint32 sent;
    QElapsedTimer timer;

    // check it isn't closed
    if (!mDevHandle || !mConnected) return -1;

    if (mDebug) {
        QString cmd, s;
        for (quint32 i=0; i<maxSize; i++) {
            cmd.append(s.sprintf("%02X:", (uchar)buf->at(i)));
        }
        cmd.remove(cmd.size()-1, 1); //remove last colon;
        qDebug() << "Sending" << maxSize << "bytes:" << cmd;
        //qDebug("Sending %ll bytes: %s", maxSize, cmd.toStdString().c_str());
    }

    sent = 0;
    sent_tmp = 0;

    timer.start();
    while (timer.elapsed() < mTimeout && maxSize-sent > 0) {
        rc = libusb_bulk_transfer(mDevHandle, (mConfig.writeEp), (uchar*)buf->data(), maxSize, &sent_tmp, mTimeout);
        if (rc == LIBUSB_ERROR_PIPE) {
            libusb_clear_halt(mDevHandle, mConfig.readEp);
        }
        sent += sent_tmp;
        if (rc != 0) break;
    }

    if ((qint32)maxSize != sent) {
        qWarning("Only sent %d out of %d", sent, maxSize);
        return -1;
    }

    if (rc != 0)
    {
        qWarning("libusb_bulk_transfer Error Writing: %d", rc);
        this->printUsbError(rc);
        return -2;
    }

    return sent;
}
