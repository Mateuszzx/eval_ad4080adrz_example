if(CONFIG_ADC)
  zephyr_library()
  zephyr_library_sources(adc_ad4080.c)
endif()
