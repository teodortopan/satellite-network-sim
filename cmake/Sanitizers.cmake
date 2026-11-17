# AddressSanitizer (with LeakSanitizer) + UndefinedBehaviorSanitizer, enabled by
# SATSIM_ENABLE_SANITIZERS. Applied to satsim targets and to GoogleTest so the
# whole test binary is instrumented consistently; GoogleTest gets only these
# flags, not satsim's warnings.
set(SATSIM_SANITIZER_FLAGS
  -fsanitize=address,undefined
  -fno-sanitize-recover=all
  -fno-omit-frame-pointer)

function(satsim_enable_sanitizers target)
  if(SATSIM_ENABLE_SANITIZERS)
    target_compile_options(${target} PRIVATE ${SATSIM_SANITIZER_FLAGS})
    target_link_options(${target} PUBLIC -fsanitize=address,undefined)
  endif()
endfunction()
