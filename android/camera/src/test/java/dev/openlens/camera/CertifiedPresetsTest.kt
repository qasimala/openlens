package dev.openlens.camera

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class CertifiedPresetsTest {
    @Test fun exactPresetWins() {
        assertEquals(CertifiedPresets.FULL_HD_1080P30, CertifiedPresets.choose(
            CertifiedPresets.FULL_HD_1080P30,
            listOf(CertifiedPresets.HD_720P30, CertifiedPresets.FULL_HD_1080P30),
        ))
    }
    @Test fun fallsBackTo720p30() {
        assertEquals(CertifiedPresets.HD_720P30, CertifiedPresets.choose(
            CertifiedPresets.FULL_HD_1080P30,
            listOf(CertifiedPresets.HD_720P30),
        ))
    }
    @Test fun noUnsupportedFallback() {
        assertNull(CertifiedPresets.choose(CertifiedPresets.FULL_HD_1080P30, emptyList()))
    }
}
