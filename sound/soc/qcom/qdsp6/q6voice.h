/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _Q6_VOICE_H
#define _Q6_VOICE_H

enum q6voice_path_type {
	Q6VOICE_PATH_VOICE= 0,
	Q6VOICE_PATH_VOIP,
	Q6VOICE_PATH_VOLTE,
	Q6VOICE_PATH_VOICE2,
	Q6VOICE_PATH_QCHAT,
	Q6VOICE_PATH_VOWLAN,
	Q6VOICE_PATH_VOICEMMODE1,
	Q6VOICE_PATH_VOICEMMODE2,
	Q6VOICE_PATH_COUNT
};

enum q6voice_port_type {
	Q6VOICE_PORT_RX = 0,
	Q6VOICE_PORT_TX,
};

struct q6voice;

struct q6voice *q6voice_create(struct device *dev);
int q6voice_start(struct q6voice *v, enum q6voice_path_type path, bool capture);
int q6voice_stop(struct q6voice *v, enum q6voice_path_type path, bool capture);

unsigned int q6voice_get_port(struct q6voice *v, enum q6voice_port_type port);
void q6voice_set_port(struct q6voice *v, enum q6voice_port_type port, int index);

#endif /*_Q6_VOICE_H */
