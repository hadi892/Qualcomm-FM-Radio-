package com.example.fm.sdk

/**
 * Data class containing standard RDS (Radio Data System) fields.
 */
data class OpenFmRds(
    val pi: Int = 0,                // Program Identification (Country + Coverage + Station Code)
    val pty: Int = 0,               // Program Type (Genre classification, 0-31)
    val ps: String? = null,         // Program Service Name (Dynamic or Static 8-char display)
    val rt: String? = null,         // Radio Text (Up to 64-character dynamic scroll text)
    val ta: Boolean = false,        // Traffic Announcement flag
    val tp: Boolean = false,        // Traffic Program flag
    val afList: List<Int> = emptyList(), // Alternative Frequencies list (KHz)
    val clockTimeMillis: Long = 0,  // Broadcast Synchronized Clock Time
    val emergencyAlert: String? = null // Emergency Alert System (EAS) messages
) {
    /**
     * Helper to return the human-readable genre name corresponding to the PTY code.
     */
    fun getPtyName(): String {
        return when (pty) {
            1 -> "News"
            2 -> "Current Affairs"
            3 -> "Information"
            4 -> "Sport"
            5 -> "Education"
            6 -> "Drama"
            7 -> "Culture"
            8 -> "Science"
            9 -> "Varied"
            10 -> "Pop Music"
            11 -> "Rock Music"
            12 -> "Easy Listening"
            13 -> "Light Classics"
            14 -> "Serious Classics"
            15 -> "Other Music"
            16 -> "Weather"
            17 -> "Finance"
            18 -> "Children's programmes"
            19 -> "Social Affairs"
            20 -> "Religion"
            21 -> "Phone In"
            22 -> "Travel"
            23 -> "Leisure"
            24 -> "Jazz Music"
            25 -> "Country Music"
            26 -> "National Music"
            27 -> "Oldies Music"
            28 -> "Folk Music"
            29 -> "Documentary"
            30 -> "Alarm Test"
            31 -> "Alarm (Emergency!)"
            else -> "Unknown/None"
        }
    }
}
