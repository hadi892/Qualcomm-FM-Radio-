package com.example

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Scaffold
import androidx.compose.ui.Modifier
import androidx.lifecycle.ViewModelProvider
import com.example.fm.data.FmDatabase
import com.example.fm.data.FmRepository
import com.example.fm.ui.FmScreen
import com.example.fm.ui.FmViewModel
import com.example.fm.ui.FmViewModelFactory
import com.example.ui.theme.MyApplicationTheme

class MainActivity : ComponentActivity() {
  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)
    enableEdgeToEdge()

    // Initialize Room Database & Repository
    val database = FmDatabase.getDatabase(this)
    val repository = FmRepository(database.fmPresetDao())

    // Create ViewModel with simple Constructor Injection via Factory
    val viewModelFactory = FmViewModelFactory(application, repository)
    val fmViewModel = ViewModelProvider(this, viewModelFactory)[FmViewModel::class.java]

    setContent {
      MyApplicationTheme {
        Scaffold(modifier = Modifier.fillMaxSize()) { innerPadding ->
          FmScreen(
            viewModel = fmViewModel,
            modifier = Modifier.padding(innerPadding)
          )
        }
      }
    }
  }
}
