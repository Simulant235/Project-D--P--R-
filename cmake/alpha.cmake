# T90, pro Alpha setzen, JJJJMMTT; leer = kein Ablauf
#   RELEASE: Datum des Builds (PC-Uhr davor = manipuliert)
#   EXPIRES: letzter gültiger Tag
set(DIVA_ALPHA_RELEASE "20261118")
set(DIVA_ALPHA_EXPIRES "20261214")

# XOR-maskiert, gleicher Key in alpha_guard.cpp
set(DIVA_ALPHA_KEY 1515870810)   # 0x5A5A5A5A
if(DIVA_ALPHA_EXPIRES)
    math(EXPR DIVA_ALPHA_EXPIRES_X "${DIVA_ALPHA_EXPIRES} ^ ${DIVA_ALPHA_KEY}")
    math(EXPR DIVA_ALPHA_RELEASE_X "${DIVA_ALPHA_RELEASE} ^ ${DIVA_ALPHA_KEY}")
else()
    set(DIVA_ALPHA_EXPIRES_X 0)
    set(DIVA_ALPHA_RELEASE_X 0)
endif()
