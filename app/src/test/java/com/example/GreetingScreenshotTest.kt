package com.example

import android.app.Application
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onRoot
import androidx.room.Room
import androidx.test.core.app.ApplicationProvider
import com.example.fm.data.FmDatabase
import com.example.fm.data.FmRepository
import com.example.fm.ui.FmScreen
import com.example.fm.ui.FmViewModel
import com.example.ui.theme.MyApplicationTheme
import com.github.takahirom.roborazzi.RobolectricDeviceQualifiers
import com.github.takahirom.roborazzi.captureRoboImage
import org.junit.After
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode

@RunWith(RobolectricTestRunner::class)
@GraphicsMode(GraphicsMode.Mode.NATIVE)
@Config(qualifiers = RobolectricDeviceQualifiers.Pixel8, sdk = [36])
class GreetingScreenshotTest {

  @get:Rule val composeTestRule = createComposeRule()

  private lateinit var db: FmDatabase
  private lateinit var repository: FmRepository
  private lateinit var viewModel: FmViewModel

  @Before
  fun setUp() {
    val context = ApplicationProvider.getApplicationContext<Application>()
    db = Room.inMemoryDatabaseBuilder(context, FmDatabase::class.java)
        .allowMainThreadQueries()
        .build()
    repository = FmRepository(db.fmPresetDao())
    viewModel = FmViewModel(context, repository)
  }

  @After
  fun tearDown() {
    db.close()
  }

  @Test
  fun greeting_screenshot() {
    composeTestRule.setContent {
      MyApplicationTheme {
        FmScreen(viewModel = viewModel)
      }
    }

    composeTestRule.onRoot().captureRoboImage(filePath = "src/test/screenshots/greeting.png")
  }
}
