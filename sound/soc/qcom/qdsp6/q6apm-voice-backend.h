/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __Q6APM_VOICE_BACKEND_H__
#define __Q6APM_VOICE_BACKEND_H__

#include <sound/soc.h>

#define Q6APM_VOICE_RX_LINK "Marble Voice Playback"
#define Q6APM_VOICE_TX_LINK "Marble Voice Capture"

static inline bool q6apm_voice_backend_link(const struct snd_soc_pcm_runtime *rtd)
{
	return !strcmp(rtd->dai_link->name, Q6APM_VOICE_RX_LINK) ||
		!strcmp(rtd->dai_link->name, Q6APM_VOICE_TX_LINK);
}

int q6apm_voice_backend_open(struct snd_soc_component *component, unsigned int graph,
			     struct snd_pcm_substream **stream);
void q6apm_voice_backend_close(struct snd_pcm_substream **stream);

#endif
