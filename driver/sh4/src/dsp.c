#include <aicaflow/dsp.h>

#include <string.h>

/* Low-level DSP image encoder; prefab programs live in dsp_prefabs.c. */
static int step_valid(const afx_dsp_step_t *s) {
    return s && s->tra <= 127 && s->twt <= 1 && s->twa <= 127 && s->xsel <= 1 &&
           s->ysel <= 3 && s->ira <= 63 && s->iwt <= 1 && s->iwa <= 31 &&
           s->table <= 1 && s->mwt <= 1 && s->mrd <= 1 && s->ewt <= 1 &&
           s->ewa <= 15 && s->adrl <= 1 && s->frcl <= 1 && s->shift <= 3 &&
           s->yrl <= 1 && s->negb <= 1 && s->zero <= 1 && s->bsel <= 1 &&
           s->nofl <= 1 && s->masa <= 31 && s->adreb <= 1 && s->nxadr <= 1;
}

int afx_dsp_program_init(afx_dsp_program_t *program) {
    if (!program) return -AFX_BAD_COMMAND;
    memset(program, 0, sizeof(*program));
    for (uint8_t i = 0; i < AFX_DSP_STEPS; ++i) {
        program->words[i * 4 + 1] = 1u << 13; /* YSEL=COEF. */
        program->words[i * 4 + 2] = 1u << 1;  /* ZERO. */
    }
    return afx_dsp_program_returns(program, 0x0f1f, 0x0f0f);
}

int afx_dsp_program_step(afx_dsp_program_t *program, uint8_t index,
                         const afx_dsp_step_t *s) {
    if (!program || index >= AFX_DSP_STEPS || !step_valid(s) || s->table ||
        ((s->mrd || s->mwt) && !(index & 1u)) || (s->ewt && s->ewa > 1))
        return -AFX_BAD_COMMAND;
    if (s->mrd || s->mwt) {
        uint16_t nofl = s->nofl;
        for (uint8_t i = 0; i < AFX_DSP_STEPS; ++i) {
            uint16_t w2 = program->words[i * 4 + 2];
            if ((w2 & 0x6000u) && i != index && ((program->words[i * 4 + 3] >> 15) & 1u) != nofl)
                return -AFX_BAD_COMMAND;
        }
    }
    uint16_t *w = program->words + index * 4;
    w[0] = (uint16_t)s->tra << 9 | (uint16_t)s->twt << 8 | (uint16_t)s->twa << 1;
    w[1] = (uint16_t)s->xsel << 15 | (uint16_t)s->ysel << 13 |
           (uint16_t)s->ira << 7 | (uint16_t)s->iwt << 6 | (uint16_t)s->iwa << 1;
    w[2] = (uint16_t)s->table << 15 | (uint16_t)s->mwt << 14 |
           (uint16_t)s->mrd << 13 | (uint16_t)s->ewt << 12 | (uint16_t)s->ewa << 8 |
           (uint16_t)s->adrl << 7 | (uint16_t)s->frcl << 6 | (uint16_t)s->shift << 4 |
           (uint16_t)s->yrl << 3 | (uint16_t)s->negb << 2 | (uint16_t)s->zero << 1 | s->bsel;
    w[3] = (uint16_t)s->nofl << 15 | (uint16_t)s->masa << 9 |
           (uint16_t)s->adreb << 8 | (uint16_t)s->nxadr << 7;
    return AFX_OK;
}

int afx_dsp_program_coefficient(afx_dsp_program_t *program, uint8_t index,
                                int16_t value) {
    if (!program || index >= AFX_DSP_COEFFICIENTS || (value & 7)) return -AFX_BAD_COMMAND;
    program->words[AFX_DSP_MPRO_WORDS + index] = (uint16_t)value;
    return AFX_OK;
}

int afx_dsp_program_address(afx_dsp_program_t *program, uint8_t index,
                            uint16_t value) {
    if (!program || index >= AFX_DSP_ADDRESSES || (index & 1u)) return -AFX_BAD_COMMAND;
    program->words[AFX_DSP_MPRO_WORDS + AFX_DSP_COEFFICIENTS + index] = value;
    return AFX_OK;
}

int afx_dsp_program_returns(afx_dsp_program_t *program, uint16_t left,
                            uint16_t right) {
    if (!program || (left | right) & ~0x0f1fu) return -AFX_BAD_COMMAND;
    program->words[AFX_DSP_PROGRAM_WORDS - 2] = left;
    program->words[AFX_DSP_PROGRAM_WORDS - 1] = right;
    return AFX_OK;
}
