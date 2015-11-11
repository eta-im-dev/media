/*
 * Copyright (C) 2008  Barracuda Networks, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301  USA
 *
 */

#include <QDialog>
#include <QMainWindow>
#include <QUdpSocket>
#include <QComboBox>
#include <QHostAddress>
#include <QFile>

#include "ui_mainwin.h"
#include "ui_config.h"

#include <psimedia.h>

class Configuration
{
public:
	bool liveInput;
	QString audioOutDeviceId, audioInDeviceId, videoInDeviceId;
	QString file;
	bool loopFile;
	PsiMedia::AudioParams audioParams;
	PsiMedia::VideoParams videoParams;

	Configuration() :
		liveInput(false),
		loopFile(false)
	{
	}
};

class ConfigDlg : public QDialog
{
	Q_OBJECT

public:
	Ui::Config ui;
	Configuration config;

	ConfigDlg(const Configuration &_config, QWidget *parent = 0);
	int findAudioParamsData(QComboBox *cb, const PsiMedia::AudioParams &params);
	int findVideoParamsData(QComboBox *cb, const PsiMedia::VideoParams &params);
	
protected:
	virtual void accept();

private slots:
	void live_toggled(bool on);
	void file_toggled(bool on);
	void file_choose();
};

// handles two udp sockets
class RtpSocketGroup : public QObject
{
	Q_OBJECT

public:
	QUdpSocket socket[2];

	RtpSocketGroup(QObject *parent = 0);
	bool bind(int basePort);
	
signals:
	void readyRead(int offset);
	void datagramWritten(int offset);

private slots:
	void sock_readyRead();
	void sock_bytesWritten(qint64 bytes);
};

// bind a channel to a socket group.
// takes ownership of socket group.
class RtpBinding : public QObject
{
	Q_OBJECT

public:
	enum Mode
	{
		Send,
		Receive
	};

	Mode mode;
	PsiMedia::RtpChannel *channel;
	RtpSocketGroup *socketGroup;
	QHostAddress sendAddress;
	int sendBasePort;

	RtpBinding(Mode _mode, PsiMedia::RtpChannel *_channel, RtpSocketGroup *_socketGroup, QObject *parent = 0);
	
private slots:
	void net_ready(int offset);
	void net_written(int offset);
	void app_ready();
	void app_written(int count);
};

class MainWin : public QMainWindow
{
	Q_OBJECT

public:
	Ui::MainWin ui;
	QAction *action_AboutProvider;
	QString creditName;
	PsiMedia::RtpSession producer;
	PsiMedia::RtpSession receiver;
	Configuration config;
	bool transmitAudio, transmitVideo, transmitting;
	bool receiveAudio, receiveVideo;
	RtpBinding *sendAudioRtp, *sendVideoRtp;
	RtpBinding *receiveAudioRtp, *receiveVideoRtp;
	bool recording;
	QFile *recordFile;

	MainWin();
	~MainWin();
	void setSendFieldsEnabled(bool b);
	void setSendConfig(const QString &s);
	void setReceiveFieldsEnabled(bool b);
	static QString rtpSessionErrorToString(PsiMedia::RtpSession::Error e);
	void cleanup_send_rtp();
	void cleanup_receive_rtp();
	void cleanup_record();
	
private slots:
	void doConfigure();
	void doAbout();
	void doAboutProvider();
	void start_send();
	void transmit();
	void stop_send();
	void start_receive();
	void stop_receive();
	void change_volume_mic(int value);
	void change_volume_spk(int value);
	void producer_started();
	void producer_stopped();
	void producer_finished();
	void producer_error();
	void receiver_started();
	void receiver_stoppedRecording();
	void receiver_stopped();
	void receiver_error();
	void record_toggle();
};
