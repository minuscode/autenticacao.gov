/*-****************************************************************************

 * Copyright (C) 2017-2021 Adriano Campos - <adrianoribeirocampos@gmail.com>
 * Copyright (C) 2017-2019 André Guerreiro - <aguerreiro1985@gmail.com>
 * Copyright (C) 2018-2020 Miguel Figueira - <miguel.figueira@caixamagica.pt>
 * Copyright (C) 2018-2019 Veniamin Craciun - <veniamin.craciun@caixamagica.pt>
 * Copyright (C) 2019 José Pinto - <jose.pinto@caixamagica.pt>
 *
 * Licensed under the EUPL V.1.2

****************************************************************************-*/

#include "gapi.h"
#include <QString>
#include <QVector>
#include <QDate>
#include <QRegExp>
#include <cstdio>
#include <QQuickImageProvider>
#include <QPrinter>
#include <QPrinterInfo>
#include "qpainter.h"

#include "CMDSignature.h"
#include "cmdErrors.h"

//SCAP
#include "scapsignature.h"
#include "SCAP-services-v3/SCAPH.h"
//#include "ASService/ASServiceH.h"   
//#include "PDFSignature/envStub.h"

#include "ScapSettings.h"
#include "credentials.h"
#include "Config.h"
#include "Util.h"
#include "MiscUtil.h"
#include "proxyinfo.h"
#include "concurrent.h"

using namespace eIDMW;

#define TRIES_LEFT_ERROR    1000

static  bool g_cleaningCallback=false;
static  int g_runningCallback=0;

/*
    GAPI - Graphic Application Programming Interface
*/

GAPI::GAPI(QObject *parent) :
    QObject(parent) {
    image_provider = new PhotoImageProvider();
    image_provider_pdf = new PDFPreviewImageProvider();

    cmd_signature = new eIDMW::CMDSignature(CMDCredentials::getCMDBasicAuthUserId(),
                                            CMDCredentials::getCMDBasicAuthPassword(), 
                                            CMDCredentials::getCMDBasicAuthAppId());
#ifdef WIN32
    m_cmdCertificates =  new eIDMW::CMDCertificates();
#endif
    m_addressLoaded = false;
    m_shortcutFlag = ShortcutIdNone;

    // Create callbacks for all readers at the startup
    setEventCallbacks();

    //----------------------------------
    // set a timer to check if the number of card readers is changed
    //----------------------------------
    m_timerReaderList = new QTimer(this);
    connect(m_timerReaderList, SIGNAL(timeout()), this, SLOT(updateReaderList()));
    m_timerReaderList->start(TIMERREADERLIST);
}

void GAPI::initTranslation() {

    QString     appPath = QCoreApplication::applicationDirPath();
    m_Settings.setExePath(appPath);
    QString CurrLng   = m_Settings.getGuiLanguageString();
    if (LoadTranslationFile(CurrLng)==false){
        emit signalLanguageChangedError();
    }
}

bool GAPI::LoadTranslationFile(QString NewLanguage)
{
    QString strTranslationFile;
    QString translations_dir;
    strTranslationFile = QString("eidmw_") + NewLanguage;

#ifdef __APPLE__
    translations_dir = m_Settings.getExePath()+"/../Resources/";
#else
    translations_dir = m_Settings.getExePath()+"/";
#endif

    qDebug() << "C++: GAPI LoadTranslationFile" << strTranslationFile << translations_dir;

    if (!m_translator.load(strTranslationFile,translations_dir))
    {
        // this should not happen, since we've built the menu with the translation filenames
        strTranslationFile = QString("eidmw_") + STR_DEF_GUILANGUAGE;
        //try load default translation file
        qDebug() << "C++: GAPI LoadTranslationFile" << strTranslationFile << translations_dir;
        if (!m_translator.load(strTranslationFile,translations_dir))
        {
            // this should not happen too, since we've built the menu with the translation filenames
            qDebug() << "C++: GAPI Load Default Translation File Error";
            return false;
        }
        qDebug() << "C++: GAPI Loaded Default Translation File";
        qApp->installTranslator(&m_translator);
        return false;
    }
    //------------------------------------
    // install the translator object and load the .qm file for
    // the given language.
    //------------------------------------
    qApp->installTranslator(&m_translator);
    return true;
}

QString GAPI::getDataCardIdentifyValue(IDInfoKey key) {

    qDebug() << "C++: getDataCardIdentifyValue ";

    return m_data[key];
}

void GAPI::setDataCardIdentify(QMap<IDInfoKey, QString> data) {

    qDebug() << "C++: setDataCardIdentify ";

    m_data = data;
    emit signalCardDataChanged();
}

bool isExpiredDate(const char * strDate) {
    if (strDate == NULL)
        return false;

    QDate qDate = QDate::fromString(strDate, "dd MM yyyy");
    QDate curDate = QDate::currentDate();

    if ( curDate > qDate )
        return true;
    return false;
}

QString GAPI::getAddressField(AddressInfoKey key) {
    return m_addressData[key];
}

#define BEGIN_TRY_CATCH  \
    try					  \
{

#define END_TRY_CATCH    \
    }                    \
    catch (PTEID_ExNoReader &) \
{                           \
    qDebug() << "No card reader found !"; \
    emit signalCardAccessError(NoReaderFound); \
    }     \
    catch (PTEID_ExNoCardPresent &) \
{     \
    qDebug() << "No card present."; \
    emit signalCardAccessError(NoCardFound); \
    }     \
    catch (PTEID_Exception &e) \
{ \
    long errorCode = e.GetError(); \
    if (errorCode >= EIDMW_SOD_UNEXPECTED_VALUE && \
    errorCode <= EIDMW_SOD_ERR_VERIFY_SOD_SIGN) \
{ \
    PTEID_LOG(PTEID_LOG_LEVEL_ERROR, "eidgui", "SOD exception! Error code (see strings in eidErrors.h): %08lx\n", e.GetError()); \
    emit signalCardAccessError(SodCardReadError); \
    } else if (errorCode == EIDMW_TIMESTAMP_ERROR) \
{ \
    emit signalPdfSignSucess(SignMessageTimestampFailed); \
    } else if (errorCode == EIDMW_LTV_ERROR) \
{ \
    emit signalPdfSignSucess(SignMessageLtvFailed); \
    } else if (errorCode == EIDMW_PERMISSION_DENIED) \
{ \
    emit signalPdfSignFail(SignFilePermissionFailed); \
    } else if (errorCode == EIDMW_PDF_UNSUPPORTED_ERROR) \
{ \
	emit signalPdfSignFail(PDFFileUnsupported); \
    } else if (errorCode == EIDMW_ERR_PIN_CANCEL) \
{ \
    emit signalCardAccessError(CardUserPinCancel); \
    } \
    else if (errorCode == EIDMW_ERR_PIN_BLOCKED) \
{ \
    emit signalCardAccessError(PinBlocked); \
    } \
    else if (errorCode == EIDMW_ERR_INCOMPATIBLE_READER) \
{ \
    emit signalCardAccessError(IncompatibleReader); \
    } \
    else if (errorCode == EIDMW_ERR_TIMEOUT) \
{ \
    emit signalCardAccessError(CardPinTimeout); \
    } \
    else \
{ \
    PTEID_LOG(PTEID_LOG_LEVEL_ERROR, "eidgui", "Generic eidlib exception! Error code (see strings in eidErrors.h): %08lx\n", e.GetError()); \
    /* Discard the 0xe1d0 prefix */                       \
    unsigned long user_error = e.GetError() & 0x0000FFFF; \
    QString msgError = QString("%1\n").arg(user_error); \
    emit signalGenericError(msgError); \
    } \
}

void GAPI::getAddressFile() {
    qDebug() << "C++: getAddressFile()";
    bool m_foreign;
    PTEID_LOG(eIDMW::PTEID_LOG_LEVEL_DEBUG, "eidgui", "GetCardInstance getAddressFile");

    BEGIN_TRY_CATCH;

    PTEID_EIDCard * card = NULL;
    getCardInstance(card);
    if (card == NULL) return;

    PTEID_Address &addressFile = card->getAddr();

    if (!addressFile.isNationalAddress())
    {
        qDebug() << "Is foreign citizen";
        m_foreign = true;
        m_addressData[Foreigncountry] = QString::fromUtf8(addressFile.getForeignCountry());
        m_addressData[Foreignaddress] = QString::fromUtf8(addressFile.getForeignAddress());
        m_addressData[Foreigncity] = QString::fromUtf8(addressFile.getForeignCity());
        m_addressData[Foreignregion] = QString::fromUtf8(addressFile.getForeignRegion());
        m_addressData[Foreignlocality] = QString::fromUtf8(addressFile.getForeignLocality());
        m_addressData[Foreignpostalcode] = QString::fromUtf8(addressFile.getForeignPostalCode());
    }
    else
    {
        qDebug() << "Is national citizen";
        m_foreign = false;
        m_addressData[District] = QString::fromUtf8(addressFile.getDistrict());
        m_addressData[Municipality] = QString::fromUtf8(addressFile.getMunicipality());
        m_addressData[Parish] = QString::fromUtf8(addressFile.getCivilParish());
        m_addressData[Streettype] = QString::fromUtf8(addressFile.getStreetType());
        m_addressData[Streetname] = QString::fromUtf8(addressFile.getStreetName());
        m_addressData[Buildingtype] = QString::fromUtf8(addressFile.getBuildingType());
        m_addressData[Doorno] = QString::fromUtf8(addressFile.getDoorNo());
        m_addressData[Floor] = QString::fromUtf8(addressFile.getFloor());
        m_addressData[Side] = QString::fromUtf8(addressFile.getSide());
        m_addressData[Locality] = QString::fromUtf8(addressFile.getLocality());
        m_addressData[Place] = QString::fromUtf8(addressFile.getPlace());
        m_addressData[Zip4] = QString::fromUtf8(addressFile.getZip4());
        m_addressData[Zip3] = QString::fromUtf8(addressFile.getZip3());
        m_addressData[PostalLocality] = QString::fromUtf8(addressFile.getPostalLocality());
    }
    emit signalAddressLoaded(m_foreign);

    END_TRY_CATCH
}

void GAPI::doSaveCardPhoto(QString outputFile) {
    PTEID_EIDCard * card = NULL;
    getCardInstance(card);
    if (card == NULL) return;

    PTEID_ByteArray& photo = card->getID().getPhotoObj().getphoto();

    QImage image_photo;
    image_photo.loadFromData(photo.GetBytes(), photo.Size(), "PNG");

    bool success = image_photo.save(outputFile);
    emit signalSaveCardPhotoFinished(success);
}

void GAPI::getPersoDataFile() {

    PTEID_LOG(eIDMW::PTEID_LOG_LEVEL_DEBUG, "eidgui", "GetCardInstance getPersoDataFile");

    BEGIN_TRY_CATCH;

    PTEID_EIDCard * card = NULL;
    getCardInstance(card);
    if (card == NULL) return;

    emit signalPersoDataLoaded(QString(card->readPersonalNotes()));

    END_TRY_CATCH

}

void GAPI::setPersoDataFile(QString text) {

    qDebug() << "setPersoDataFile() called";

    int testAuthPin = doVerifyAuthPin("");

    if (testAuthPin == 0 || testAuthPin == TRIES_LEFT_ERROR) {
        return;
    }

    try {
        QString TxtPersoDataString = text.toUtf8();

        PTEID_EIDCard * Card = NULL;
        getCardInstance(Card);

        const PTEID_ByteArray oData(reinterpret_cast<const unsigned char*>
            (TxtPersoDataString.toStdString().c_str()), (TxtPersoDataString.toStdString().size() + 1));
        Card->writePersonalNotes(oData);


        qDebug() << "Personal notes successfully written!";
        emit signalSetPersoDataFile(tr("STR_POPUP_SUCESS"), tr("STR_PERSONAL_NOTES_SUCESS"), true);

    } catch (PTEID_Exception& e) {
        qDebug() << "Error writing personal notes!";
        emit signalSetPersoDataFile(tr("STR_POPUP_ERROR"), tr("STR_PERSONAL_NOTES_ERROR"), false);
    }

}

void GAPI::verifyAuthPin(QString pin_value) {
    Concurrent::run(this, &GAPI::doVerifyAuthPin, pin_value);
}
unsigned int  GAPI::doVerifyAuthPin(QString pin_value) {
    setAppAsDlgParent();
    unsigned long tries_left = TRIES_LEFT_ERROR;
    PTEID_LOG(eIDMW::PTEID_LOG_LEVEL_DEBUG, "eidgui", "GetCardInstance doVerifyAuthPin");

    BEGIN_TRY_CATCH

        PTEID_EIDCard * card = NULL;
    getCardInstance(card);
    if (card == NULL) return TRIES_LEFT_ERROR;

    PTEID_Pin & auth_pin = card->getPins().getPinByPinRef(PTEID_Pin::AUTH_PIN);
    auth_pin.verifyPin(pin_value.toLatin1().data(), tries_left);

    if (tries_left == 0) {
        qDebug() << "WARNING: Auth PIN blocked!" << tries_left;
    }

    END_TRY_CATCH

        emit signalTestPinFinished(tries_left, AuthPin);
    //QML default types don't include long
    return (unsigned int)tries_left;
}

void GAPI::getTriesLeftAuthPin() {
    Concurrent::run(this, &GAPI::doGetTriesLeftAuthPin);
}
unsigned int GAPI::doGetTriesLeftAuthPin() {
    setAppAsDlgParent();
    unsigned long tries_left = TRIES_LEFT_ERROR;
    PTEID_LOG(eIDMW::PTEID_LOG_LEVEL_DEBUG, "eidgui", "GetCardInstance doGetTriesLeftAuthPin");

    BEGIN_TRY_CATCH

        PTEID_EIDCard * card = NULL;
    getCardInstance(card);
    if (card == NULL) return TRIES_LEFT_ERROR;

    PTEID_Pin & auth_pin = card->getPins().getPinByPinRef(PTEID_Pin::AUTH_PIN);
    tries_left = auth_pin.getTriesLeft();

    if (tries_left == 0) {
        qDebug() << "WARNING: Auth PIN blocked!" << tries_left;
    }

    END_TRY_CATCH

        emit signalTriesLeftPinFinished(tries_left, AuthPin);
    //QML default types don't include long
    return (unsigned int)tries_left;
}

void GAPI::verifySignPin(QString pin_value) {
    Concurrent::run(this, &GAPI::doVerifySignPin, pin_value);
}
unsigned int  GAPI::doVerifySignPin(QString pin_value) {
    setAppAsDlgParent();
    unsigned long tries_left = TRIES_LEFT_ERROR;
    PTEID_LOG(eIDMW::PTEID_LOG_LEVEL_DEBUG, "eidgui", "GetCardInstance doVerifySignPin");

    BEGIN_TRY_CATCH

        PTEID_EIDCard * card = NULL;
    getCardInstance(card);
    if (card == NULL) return TRIES_LEFT_ERROR;

    PTEID_Pin & sign_pin = card->getPins().getPinByPinRef(PTEID_Pin::SIGN_PIN);
    sign_pin.verifyPin(pin_value.toLatin1().data(), tries_left);

    if (tries_left == 0) {
        qDebug() << "WARNING: Sign PIN blocked!" << tries_left;
    }

    END_TRY_CATCH

        emit signalTestPinFinished(tries_left, SignPin);
    //QML default types don't include long
    return (unsigned int)tries_left;
}

void GAPI::getTriesLeftSignPin() {
    Concurrent::run(this, &GAPI::doGetTriesLeftSignPin);
}
unsigned int GAPI::doGetTriesLeftSignPin() {
    setAppAsDlgParent();
    unsigned long tries_left = TRIES_LEFT_ERROR;
    PTEID_LOG(eIDMW::PTEID_LOG_LEVEL_DEBUG, "eidgui", "GetCardInstance doGetTriesLeftSignPin");

    BEGIN_TRY_CATCH

        PTEID_EIDCard * card = NULL;
    getCardInstance(card);
    if (card == NULL) return TRIES_LEFT_ERROR;

    PTEID_Pin & sign_pin = card->getPins().getPinByPinRef(PTEID_Pin::SIGN_PIN);
    tries_left = sign_pin.getTriesLeft();

    if (tries_left == 0) {
        qDebug() << "WARNING: Sign PIN blocked!" << tries_left;
    }

    END_TRY_CATCH

        emit signalTriesLeftPinFinished(tries_left, SignPin);
    //QML default types don't include long
    return (unsigned int)tries_left;
}

void GAPI::verifyAddressPin(QString pin_value) {
    Concurrent::run(this, &GAPI::doVerifyAddressPin, pin_value);
}
unsigned int GAPI::doVerifyAddressPin(QString pin_value) {
    setAppAsDlgParent();
    unsigned long tries_left = TRIES_LEFT_ERROR;
    PTEID_LOG(eIDMW::PTEID_LOG_LEVEL_DEBUG, "eidgui", "GetCardInstance doVerifyAddressPin");

    BEGIN_TRY_CATCH

        PTEID_EIDCard * card = NULL;
    getCardInstance(card);
    if (card == NULL) return TRIES_LEFT_ERROR;

    PTEID_Pin & address_pin = card->getPins().getPinByPinRef(PTEID_Pin::ADDR_PIN);
    address_pin.verifyPin(pin_value.toLatin1().data(), tries_left);

    if (tries_left == 0) {
        qDebug() << "WARNING: Address PIN blocked!" << tries_left;
    }

    END_TRY_CATCH

        emit signalTestPinFinished(tries_left, AddressPin);
    //QML default types don't include long
    return (unsigned int)tries_left;
}

void GAPI::getTriesLeftAddressPin() {
    Concurrent::run(this, &GAPI::doGetTriesLeftAddressPin);
}
unsigned int GAPI::doGetTriesLeftAddressPin() {
    setAppAsDlgParent();
    unsigned long tries_left = TRIES_LEFT_ERROR;
    PTEID_LOG(eIDMW::PTEID_LOG_LEVEL_DEBUG, "eidgui", "GetCardInstance doGetTriesLeftAddressPin");

    BEGIN_TRY_CATCH

        PTEID_EIDCard * card = NULL;
    getCardInstance(card);
    if (card == NULL) return TRIES_LEFT_ERROR;

    PTEID_Pin & address_pin = card->getPins().getPinByPinRef(PTEID_Pin::ADDR_PIN);
    tries_left = address_pin.getTriesLeft();

    if (tries_left == 0) {
        qDebug() << "WARNING: Address PIN blocked!" << tries_left;
    }

    END_TRY_CATCH

        emit signalTriesLeftPinFinished(tries_left, AddressPin);
    //QML default types don't include long
    return (unsigned int)tries_left;
}

void GAPI::changeAuthPin(QString currentPin, QString newPin) {
    Concurrent::run(this, &GAPI::doChangeAuthPin, currentPin, newPin);
}
unsigned int GAPI::doChangeAuthPin(QString currentPin, QString newPin) {
    setAppAsDlgParent();
    unsigned long tries_left = TRIES_LEFT_ERROR;
    PTEID_LOG(eIDMW::PTEID_LOG_LEVEL_DEBUG, "eidgui", "GetCardInstance doChangeAuthPin");

    BEGIN_TRY_CATCH

        PTEID_EIDCard * card = NULL;
    getCardInstance(card);
    if (card == NULL) return TRIES_LEFT_ERROR;

    PTEID_Pin & auth_pin = card->getPins().getPinByPinRef(PTEID_Pin::AUTH_PIN);
    auth_pin.changePin(currentPin.toLatin1().data(), newPin.toLatin1().data(), tries_left, "");

    if (tries_left == 0) {
        qDebug() << "WARNING: Auth PIN blocked!" << tries_left;
    }

    END_TRY_CATCH

        emit signalModifyPinFinished(tries_left, AuthPin);
    //QML default types don't include long
    return (unsigned int)tries_left;
}

void GAPI::changeSignPin(QString currentPin, QString newPin) {
    Concurrent::run(this, &GAPI::doChangeSignPin, currentPin, newPin);
}
unsigned int GAPI::doChangeSignPin(QString currentPin, QString newPin) {
    setAppAsDlgParent();
    unsigned long tries_left = TRIES_LEFT_ERROR;
    PTEID_LOG(eIDMW::PTEID_LOG_LEVEL_DEBUG, "eidgui", "GetCardInstance doChangeSignPin");

    BEGIN_TRY_CATCH

        PTEID_EIDCard * card = NULL;
    getCardInstance(card);
    if (card == NULL) return TRIES_LEFT_ERROR;

    PTEID_Pin & sign_pin = card->getPins().getPinByPinRef(PTEID_Pin::SIGN_PIN);
    sign_pin.changePin(currentPin.toLatin1().data(), newPin.toLatin1().data(), tries_left, "");

    if (tries_left == 0) {
        qDebug() << "WARNING: Sign PIN blocked!" << tries_left;
    }

    END_TRY_CATCH

        emit signalModifyPinFinished(tries_left, SignPin);
    //QML default types don't include long
    return (unsigned int)tries_left;
}
void GAPI::showChangeAddressDialog(long code)
{
    QString error_msg;
    long sam_error_code = 0;
    QString support_string = tr("STR_CHANGE_ADDRESS_ERROR_MSG");
    QString support_string_wait_5min = tr("STR_CHANGE_ADDRESS_WAIT_5MIN_ERROR_MSG");

    if (code == 0){
        PTEID_LOG(PTEID_LOG_LEVEL_CRITICAL, "eidgui", "AddressChange op finished with sucess");
    } else {
		PTEID_LOG(PTEID_LOG_LEVEL_ERROR, "eidgui", "AddressChange op finished with error code 0x%08x", code);
    }

    switch (code)
    {
    case 0:
        error_msg = tr("STR_CHANGE_ADDRESS_SUCESS");
        //Reload address data in case of successful address change
        startReadingAddress();
        break;
        //The error code for connection error is common between SAM and OTP
    case EIDMW_OTP_CONNECTION_ERROR:
        error_msg = "<b>" + tr("STR_CHANGE_ADDRESS_ERROR") + "</b><br><br>" + tr("STR_CONNECTION_ERROR") + "<br><br>" +
            tr("STR_VERIFY_INTERNET");
        if (m_Settings.isProxyConfigured())
            error_msg.append(" ").append(tr("STR_VERIFY_PROXY"));
        break;

    case EIDMW_OTP_CERTIFICATE_ERROR:
        error_msg = "<b>" + tr("STR_CHANGE_ADDRESS_ERROR") + "</b><br><br>" + tr("STR_CONNECTION_ERROR") + "<br><br>" +
            tr("STR_VERIFY_APP_UPDATE");
        break;

    case EIDMW_SAM_PROXY_AUTH_FAILED:
        error_msg = "<b>" + tr("STR_CHANGE_ADDRESS_ERROR") + "</b><br><br>" + tr("STR_CONNECTION_ERROR") + "<br><br>" +
            tr("STR_PROXY_AUTH_FAILED");
        break;
    case EIDMW_SAM_PROXY_UNSUPPORTED:
        error_msg = "<b>" + tr("STR_CHANGE_ADDRESS_ERROR") + "</b><br><br>" + tr("STR_CONNECTION_ERROR") + "<br><br>" +
            tr("STR_PROXY_UNSUPPORTED");
        break;
    case SAM_PROCESS_NUMBER_ERROR_1:
    case SAM_PROCESS_NUMBER_ERROR_2:
        error_msg = "<b>" + tr("STR_CHANGE_ADDRESS_ERROR") + "</b><br><br>" + tr("STR_CHANGE_ADDRESS_CHECK_PROCESS_NUMBER");
        sam_error_code = code;
        break;
    case SAM_PROCESS_EXPIRED_ERROR:
        error_msg = "<b>" + tr("STR_CHANGE_ADDRESS_ERROR") + "</b><br><br>" + tr("STR_CHANGE_ADDRESS_CHECK_PROCESS_EXPIRED");
        sam_error_code = code;
        break;
    case EIDMW_SAM_UNCONFIRMED_CHANGE:
        error_msg = "<b>" + tr("STR_CHANGE_ADDRESS_ERROR") + "</b><br><br>" + tr("STR_CHANGE_ADDRESS_ERROR_INCOMPLETE") + "<br><br>" + tr("STR_CHANGE_ADDRESS_NOT_CONFIRMED");
        break;
    case EIDMW_SSL_PROTOCOL_ERROR:
        error_msg = "<b>" + tr("STR_CHANGE_ADDRESS_ERROR") + "</b><br><br>" + tr("STR_CHANGE_ADDRESS_CHECK_AUTHENTICATION_CERTIFICATE");
        break;
    default:
        //Make sure we only show the user error codes from the SAM service and not some weird pteid exception error code
        if (code > 1100 && code < 3500)
            sam_error_code = code;
        error_msg = "<b>" + tr("STR_CHANGE_ADDRESS_ERROR") + "</b><br><br>" + "\n\n";
        break;
    }

    if (sam_error_code != 0)
    {
        error_msg += "<br><br>" + tr("STR_ERROR_CODE") + QString::number(sam_error_code);
    }

    if (code == EIDMW_SAM_UNCONFIRMED_CHANGE){
        error_msg += "<br><br>" + support_string_wait_5min;
    } else if (code != 0 && code != SAM_PROCESS_EXPIRED_ERROR){
        error_msg += "<br><br>" + support_string;
        signalAddressShowLink();
    }

    qDebug() << error_msg;
    signalUpdateProgressStatus(error_msg);
}

void GAPI::showSignCMDDialog(long error_code)
{
    QString message;
    QString support_string = tr("STR_CMD_ERROR_MSG");
    QString urlLink = tr("STR_MAIL_SUPPORT");

    switch (error_code)
    {
    case 0:
        message = tr("STR_CMD_SUCESS");
        break;
    case SCAP_GENERIC_ERROR_CODE:
        message = tr("STR_SCAP_SIGNATURE_ERROR");
        break;
    case SCAP_CLOCK_ERROR_CODE:
        message = tr("STR_SCAP_CLOCK_ERROR");
        break;
    case SCAP_SECRETKEY_ERROR_CODE:
        message = tr("STR_SCAP_SECRETKEY_ERROR");
        break;
    case SCAP_ATTR_POSSIBLY_EXPIRED_WARNING:
        message = tr("STR_SCAP_SIGNATURE_ERROR") + "<br>" + tr("STR_SCAP_CHECK_EXPIRED_ATTR");
        break;
    case SCAP_ATTRIBUTES_EXPIRED:
        message = tr("STR_SCAP_NOT_VALID_ATTRIBUTES");
        break;
    case SCAP_ZERO_ATTRIBUTES:
        message = tr("STR_SCAP_NOT_VALID_ATTRIBUTES");
        break;
    case SCAP_ATTRIBUTES_NOT_VALID:
        message = tr("STR_SCAP_NOT_VALID_ATTRIBUTES");
        break;
    case SOAP_EOF:
        message = tr("STR_CMD_TIMEOUT_ERROR");
        break;
    case ERR_GET_CERTIFICATE:
        urlLink = tr("STR_URL_AUTENTICACAO_GOT_PT");
        message = tr("STR_CMD_GET_CERTIFICATE_ERROR");
        support_string = "";
        break;
    case HTTP_PROXY_AUTH_REQUIRED:
        message = tr("STR_CMD_PROXY_AUTH_ERROR");
        break;
    case SOAP_TCP_ERROR:
        message = tr("STR_CONNECTION_ERROR") + "<br><br>" +
            tr("STR_VERIFY_INTERNET");
        if (m_Settings.isProxyConfigured())
            message.append(" ").append(tr("STR_VERIFY_PROXY"));
        break;
    case SOAP_ERR_SERVICE_FAIL:
        message = tr("STR_CMD_SERVICE_FAIL");
        break;
    case SOAP_ERR_INVALID_PIN:
        message = tr("STR_CMD_INVALID_PIN");
        break;
    case SOAP_ERR_INVALID_OTP:
        message = tr("STR_CMD_INVALID_OTP");
        break;
    case SOAP_ERR_OTP_VALIDATION_ERROR:
        message = tr("STR_CMD_OTP_VALIDATION_ERROR");
        break;
    case SOAP_ERR_INACTIVE_SERVICE:
        message = tr("STR_CMD_INACTIVE_SERVICE");
        break;
    case EIDMW_PERMISSION_DENIED:
        message = tr("STR_SIGN_FILE_PERMISSION_FAIL");
        break;
    case EIDMW_TIMESTAMP_ERROR:
        message = tr("STR_CMD_SUCESS") + " " + tr("STR_TIME_STAMP_FAILED");
        break;
    case EIDMW_LTV_ERROR:
        message = tr("STR_CMD_SUCESS") + " " + tr("STR_LTV_FAILED");
        break;
    default:
        message = tr("STR_CMD_LOGIN_ERROR");
        break;
    }

    if (error_code != 0){
        // If there is error show message screen
        if (error_code == EIDMW_TIMESTAMP_ERROR || error_code == EIDMW_LTV_ERROR){
            signalUpdateProgressStatus(message);
        } 
        else if (error_code == SCAP_SECRETKEY_ERROR_CODE
                 || error_code == SCAP_ATTRIBUTES_EXPIRED
                 || error_code == SCAP_ZERO_ATTRIBUTES
                 || error_code == SCAP_ATTRIBUTES_NOT_VALID
                 || error_code == SCAP_ATTR_POSSIBLY_EXPIRED_WARNING){
            signalUpdateProgressStatus(tr("STR_POPUP_ERROR") + "! " + message);
            signalShowLoadAttrButton();
        }
        else if (error_code == SCAP_CLOCK_ERROR_CODE){
            signalUpdateProgressStatus(tr("STR_POPUP_ERROR") + "!");
            signalShowMessage(message,"");
        } else {
            message += "<br><br>" + support_string;
            signalUpdateProgressStatus(tr("STR_POPUP_ERROR") + "!");
            signalShowMessage(message, urlLink);
        }
    }
    else{
        // Success
        signalUpdateProgressStatus(message);
    }

    if (error_code == 0 || error_code == EIDMW_TIMESTAMP_ERROR || error_code == EIDMW_LTV_ERROR){
        PTEID_LOG(PTEID_LOG_LEVEL_CRITICAL, "eidgui", 
            "CMD signature op finished with sucess with error code 0x%08x", error_code);
        emit signalOpenFile();
    } else {
        PTEID_LOG(PTEID_LOG_LEVEL_ERROR, "eidgui", 
            "CMD signature op finished with error code 0x%08x", error_code);
    }

    qDebug() << "Show Sign CMD Dialog - Error code: " << error_code
             << "Message: " << message;
}

void GAPI::changeAddressPin(QString currentPin, QString newPin) {
    Concurrent::run(this, &GAPI::doChangeAddressPin, currentPin, newPin);
}
unsigned int GAPI::doChangeAddressPin(QString currentPin, QString newPin) {
    unsigned long tries_left = TRIES_LEFT_ERROR;
    PTEID_LOG(eIDMW::PTEID_LOG_LEVEL_DEBUG, "eidgui", "GetCardInstance doChangeAddressPin");

    BEGIN_TRY_CATCH

        PTEID_EIDCard * card = NULL;
    getCardInstance(card);
    if (card == NULL) return TRIES_LEFT_ERROR;

    PTEID_Pin & address_pin = card->getPins().getPinByPinRef(PTEID_Pin::ADDR_PIN);
    address_pin.changePin(currentPin.toLatin1().data(), newPin.toLatin1().data(), tries_left, "");

    if (tries_left == 0) {
        qDebug() << "WARNING: Address PIN blocked!" << tries_left;
    }

    END_TRY_CATCH

        emit signalModifyPinFinished(tries_left, AddressPin);
    //QML default types don't include long
    return (unsigned int)tries_left;
}

void GAPI::addressChangeCallback(void *instance, int value)
{
    qDebug() << "addressChangeCallback";
    GAPI * gapi = (GAPI*)(instance);
    gapi->signalUpdateProgressBar(value);
}

void GAPI::doChangeAddress(const char *process, const char *secret_code)
{
    qDebug() << "DoChangeAddress!";
    PTEID_LOG(PTEID_LOG_LEVEL_CRITICAL, "eidgui", "Change Address started");

    try
    {
        PTEID_EIDCard * card = NULL;
        getCardInstance(card);
        if (card == NULL) return;

        card->ChangeAddress((char *)secret_code, (char*)process, &GAPI::addressChangeCallback, (void*)this);
    }
    catch (PTEID_Exception & exception)
    {
        PTEID_LOG(PTEID_LOG_LEVEL_ERROR, "eidgui", "Caught exception in EidCard.ChangeAddress()... closing progressBar");
        this->signalUpdateProgressBar(100);
        this->addressChangeFinished(exception.GetError());

        free((char *)process);
        free((char *)secret_code);
        return;
    }

    free((char *)process);
    free((char *)secret_code);
    this->addressChangeFinished(0);
}

void GAPI::changeAddress(QString process, QString secret_code)
{
    qDebug() << "ChangeAddress! process = " + process + " secret_code = " + secret_code;

    signalUpdateProgressStatus(tr("STR_CHANGING_ADDRESS"));

    connect(this, SIGNAL(addressChangeFinished(long)),
        this, SLOT(showChangeAddressDialog(long)), Qt::UniqueConnection);

    char *processUtf8 = strdup(process.toUtf8().constData());
    char *secret_codeUtf8 = strdup(secret_code.toUtf8().constData());
    Concurrent::run(this, &GAPI::doChangeAddress, processUtf8, secret_codeUtf8);
}

void GAPI::doOpenSignCMD(CMDSignature *cmd_signature, CmdParams &cmdParams, SignParams &signParams)
{
    /*qDebug() << "doOpenSignCMD! MobileNumber = " << cmdParams.mobileNumber << " secret_code = " << cmdParams.secret_code <<
    " loadedFilePaths = " << signParams.loadedFilePaths << " outputFile = " << signParams.outputFile <<
    "page = " << signParams.page << "coord_x" << signParams.coord_x << "coord_y" << signParams.coord_y <<
    "reason = " << signParams.reason << "location = " << signParams.location;*/

    int ret = -1;

    BEGIN_TRY_CATCH
        signalUpdateProgressBar(25);
    CMDProxyInfo proxyInfo = CMDProxyInfo::buildProxyInfo();
    ret = cmd_signature->signOpen(proxyInfo, cmdParams.mobileNumber.toStdString(), cmdParams.secret_code.toStdString(),
        signParams.page,
        signParams.coord_x, signParams.coord_y,
        signParams.location.toUtf8().data(), signParams.reason.toUtf8().data(),
        getPlatformNativeString(signParams.outputFile));

    if (ret != 0) {
        qDebug() << "signOpen failed! - ret: " << ret << endl;

        signCMDFinished(ret);
        signalUpdateProgressBar(100);
        return;
    }

    END_TRY_CATCH

        if (ret == -1) {
            //Exception was thrown in signOpen
            signalUpdateProgressBar(100);
            return;
        }

    signalUpdateProgressBar(50);
    signalUpdateProgressStatus(tr("STR_CMD_LOGIN_SUCESS"));
    emit signalValidateOtp();
}

void GAPI::doCloseSignCMD(CMDSignature *cmd_signature, QString sms_token)
{

    long ret = 0;
    std::string local_sms_token = sms_token.toUtf8().data();

    try {
        signalUpdateProgressBar(75);
        ret = cmd_signature->signClose(local_sms_token);

        for (size_t i = 0; i < cmd_pdfSignatures.size(); i++)
            delete cmd_pdfSignatures[i];

        cmd_pdfSignatures.clear();
        cmd_signature->clear_pdf_handlers();

        if (ret != 0) {
            qDebug() << "signClose failed!" << endl;
            signCMDFinished(ret);
            signalUpdateProgressBar(100);
            return;
        }

    } catch (PTEID_Exception &e) {
        ret = e.GetError();
        PTEID_LOG(eIDMW::PTEID_LOG_LEVEL_ERROR, "doCloseSignCMD",
            "Caught exception in some SDK method. Error code: %08x", e.GetError());
    }

    signCMDFinished(ret);
    signalUpdateProgressBar(100);
}

void GAPI::doCloseSignCMDWithSCAP(CMDSignature *cmd_signature, QString sms_token, QList<int> attribute_list) {

    long ret = 0;
    std::string local_sms_token = sms_token.toUtf8().data();

    try 
    {
        signalUpdateProgressBar(65);
        ret = cmd_signature->signClose(local_sms_token);
    }
    catch (PTEID_Exception &e) {
        ret = e.GetError();
        PTEID_LOG(eIDMW::PTEID_LOG_LEVEL_ERROR, "doCloseSignCMDWithSCAP",
            "CMD SignClose. Error code: %08x", e.GetError());
    }

    // CMD signature success
    if (ret == 0 || ret == EIDMW_TIMESTAMP_ERROR || ret == EIDMW_LTV_ERROR)
    {
        try 
        {
            signalUpdateProgressStatus(tr("STR_CMD_SIGNING_SCAP"));

            signalUpdateProgressBar(80);

            //Do SCAP Signature
            std::vector<int> attrs;
            for (int i = 0; i != attribute_list.size(); i++) {
                attrs.push_back(attribute_list.at(i));
            }

            //See details of this
            CmdSignedFileDetails cmd_details;
            cmd_details.signedCMDFile = m_scap_params.inputPDF;

            cmd_details.citizenName = cmd_signature->getCertificateCitizenName();
            //The method returns something like "BI123456789";
            cmd_details.citizenId = QString(cmd_signature->getCertificateCitizenID() + 2);

            int ret_scap = scapServices.executeSCAPWithCMDSignature(this, m_scap_params.outputPDF, m_scap_params.page,
                m_scap_params.location_x, m_scap_params.location_y,
                m_scap_params.location, m_scap_params.reason, m_scap_params.isTimestamp, m_scap_params.isLtv, attrs, cmd_details,
                useCustomSignature(), m_jpeg_scaled_data);

            for (size_t i = 0; i < cmd_pdfSignatures.size(); i++)
                delete cmd_pdfSignatures[i];

            cmd_pdfSignatures.clear();
            cmd_signature->clear_pdf_handlers();

            // SCAP signature with errors
            if(ret_scap != GAPI::ScapSucess){
                signalUpdateProgressBar(100);
                return;
            } 
        }
        catch (PTEID_Exception &e) {
            ret = e.GetError();
            PTEID_LOG(eIDMW::PTEID_LOG_LEVEL_ERROR, "doCloseSignCMDWithSCAP",
                    "executeSCAPWithCMDSignature. Error code: %08x", e.GetError());
            }
    }
    //TODO: reset the m_scap_params struct

    signCMDFinished(ret);
    signalUpdateProgressBar(100);

}


QString generateTempFile() {
    QTemporaryFile tempFile;

    if (!tempFile.open()) {
        PTEID_LOG(eIDMW::PTEID_LOG_LEVEL_DEBUG, "generateTempFile", "SCAP Signature error: Error creating temporary file");
        return "";
    }

    return tempFile.fileName();
}

void GAPI::signOpenScapWithCMD(QString mobileNumber, QString secret_code, QList<QString> loadedFilePaths,
    QString outputFile, int page, double coord_x, double coord_y,
    QString reason, QString location, bool isTimestamp, bool isLtv) {

    qDebug() << "signOpenScapWithCMD! MobileNumber = " << mobileNumber << " secret_code = " << secret_code <<
        " loadedFilePaths = " << loadedFilePaths <<
        " outputFile = " << outputFile << " page = " << page << " coord_x" << coord_x <<
        " coord_y" << coord_y << " reason = " << reason << " location = " << location << "isLtv= "<<isLtv;

    signalUpdateProgressStatus(tr("STR_CMD_CONNECTING"));

    connect(this, SIGNAL(signCMDFinished(long)),
        this, SLOT(showSignCMDDialog(long)), Qt::UniqueConnection);

    //Final params for the SCAP signature (visible PDF signature params)  
    m_scap_params.outputPDF = outputFile;
    m_scap_params.inputPDF = generateTempFile();
    m_scap_params.page = page;
    m_scap_params.location_x = coord_x;
    m_scap_params.location_y = coord_y;
    m_scap_params.location = location;
    m_scap_params.reason = reason;
    m_scap_params.isTimestamp = isTimestamp;
	m_scap_params.isLtv = isLtv;

    CmdParams cmdParams;
    SignParams signParams;

    /* Invisible CMD signature: with SCAP only the last signature generated
	   by the actual SCAP system is visible */
    cmdParams.secret_code = secret_code;
    cmdParams.mobileNumber = mobileNumber;
    signParams.loadedFilePaths = loadedFilePaths;
    signParams.outputFile = m_scap_params.inputPDF;
    signParams.page = 0;
    signParams.coord_x = -1;
    signParams.coord_y = -1;
    signParams.location = location;
    signParams.reason = reason;
    signParams.isTimestamp = false;
    signParams.isLtv = false;
    signParams.isSmallSignature = 0;

    cmd_signature->clear_pdf_handlers();

    for (int i = 0; i < loadedFilePaths.size(); i++) {
        QString fullInputPath = loadedFilePaths[i];
        PTEID_PDFSignature * cmd_pdfSignature = new eIDMW::PTEID_PDFSignature();
		
        cmd_pdfSignatures.push_back(cmd_pdfSignature); // keep track of pointers to be deleted
        cmd_pdfSignature->setFileSigning((char *)getPlatformNativeString(fullInputPath));
        PTEID_SignatureLevel sig_level = isTimestamp ?
                (isLtv ? PTEID_LEVEL_LT : PTEID_LEVEL_TIMESTAMP) : PTEID_LEVEL_BASIC;
        cmd_pdfSignature->setSignatureLevel(sig_level);
        cmd_signature->add_pdf_handler(cmd_pdfSignature);
    }

    Concurrent::run(this, &GAPI::doOpenSignCMD, cmd_signature, cmdParams, signParams);

}

void GAPI::doCancelCMDSign() {
    cmd_signature->cancelRequest();
}
void GAPI::cancelCMDSign() {
    Concurrent::run(this, &GAPI::doCancelCMDSign);
}
void GAPI::signOpenCMD(QString mobileNumber, QString secret_code, QList<QString> loadedFilePaths,
    QString outputFile, int page, double coord_x, double coord_y,
    QString reason, QString location, bool isTimestamp, bool isLTV, bool isSmall)
{
    /*qDebug() << "signOpenCMD! MobileNumber = " << mobileNumber << " secret_code = " << secret_code <<
    " loadedFilePaths = " << loadedFilePaths <<
    " outputFile = " << outputFile << " page = " << page << " coord_x" << coord_x <<
    " coord_y" << coord_y << " reason = " << reason << " location = " << location <<
    " isTimestamp = " <<  isTimestamp << " isSmall = " << isSmall;*/


    signalUpdateProgressStatus(tr("STR_CMD_CONNECTING"));

    connect(this, SIGNAL(signCMDFinished(long)),
        this, SLOT(showSignCMDDialog(long)), Qt::UniqueConnection);

    CmdParams cmdParams = { mobileNumber, secret_code };
    SignParams signParams = { loadedFilePaths, outputFile, page, coord_x,
        coord_y, reason, location, isTimestamp, isLTV, isSmall };

    cmd_signature->clear_pdf_handlers();

    for (int i = 0; i < loadedFilePaths.size(); i++) {
        QString fullInputPath = loadedFilePaths[i];
        PTEID_PDFSignature * cmd_pdfSignature = new eIDMW::PTEID_PDFSignature();

        cmd_pdfSignatures.push_back(cmd_pdfSignature); // keep track of pointers to be deleted
        cmd_pdfSignature->setFileSigning((char *)getPlatformNativeString(fullInputPath));

        if (signParams.isTimestamp) {
            if (signParams.isLtv)
            {
                cmd_pdfSignature->setSignatureLevel(PTEID_LEVEL_LTV);
            }
            else
            {
                cmd_pdfSignature->setSignatureLevel(PTEID_LEVEL_TIMESTAMP);
            }
        }

        if (signParams.isSmallSignature)
            cmd_pdfSignature->enableSmallSignatureFormat();

        if (useCustomSignature()) {
            const PTEID_ByteArray imageData(reinterpret_cast<const unsigned char *>(m_jpeg_scaled_data.data()), static_cast<unsigned long>(m_jpeg_scaled_data.size()));
            cmd_pdfSignature->setCustomImage(imageData);
        }

        cmd_signature->add_pdf_handler(cmd_pdfSignature);
    }
    Concurrent::run(this, &GAPI::doOpenSignCMD, cmd_signature, cmdParams, signParams);
}

void GAPI::signCloseCMD(QString sms_token, QList<int> attribute_list)
{
    /*qDebug() << "signCloseCMD! sms_token = " + sms_token;*/

    signalUpdateProgressStatus(tr("STR_CMD_SENDING_CODE"));

    if (attribute_list.size() > 0)
        Concurrent::run(this, &GAPI::doCloseSignCMDWithSCAP, cmd_signature, sms_token, attribute_list);

    else
        Concurrent::run(this, &GAPI::doCloseSignCMD, cmd_signature, sms_token);
}

void GAPI::sendSmsCmd(CmdDialogClass dialogType) {
    Concurrent::run(this, &GAPI::doSendSmsCmd, dialogType);
}
void GAPI::doSendSmsCmd(CmdDialogClass dialogType) {
#ifdef WIN32
    if (dialogType == GAPI::RegisterCert)
    {
        m_cmdCertificates->sendSms();
    }
    else if (dialogType == GAPI::Sign)
    {
        cmd_signature->sendSms();
    }
#else
    cmd_signature->sendSms();
#endif
    // TODO: update status
}

QString GAPI::getCardActivation() {
    PTEID_LOG(eIDMW::PTEID_LOG_LEVEL_DEBUG, "eidgui", "GetCardInstance getCardActivation");

    BEGIN_TRY_CATCH

        PTEID_EIDCard * card = NULL;
    getCardInstance(card);
    if (card == NULL) return "Error getting Card Activation";

    PTEID_EId &eid_file = card->getID();

    PTEID_Certificates&	 certificates = card->getCertificates();

    int certificateStatus = PTEID_CERTIF_STATUS_UNKNOWN;

    certificateStatus = certificates.getCert(PTEID_Certificate::CITIZEN_AUTH).getStatus();

    PTEID_LOG(eIDMW::PTEID_LOG_LEVEL_DEBUG, "eidgui", "PTEID_Certificate::getStatus() returned: %d", certificateStatus);

    // If state active AND validity not expired AND certificate active
    if (card->isActive() && !isExpiredDate(eid_file.getValidityEndDate())
        && certificateStatus == PTEID_CERTIF_STATUS_VALID)
    {
        return QString(tr("STR_CARD_ACTIVE_AND_VALID"));
    }
    // Else If state active AND validity not expired AND certificate validation error
    else if (card->isActive() && !isExpiredDate(eid_file.getValidityEndDate()))
    {
        //Network access errors (OCSP/CRL)
        if (certificateStatus == PTEID_CERTIF_STATUS_CONNECT) {
            QString msg(tr("STR_CARD_CONNECTION_ERROR"));
            if (m_Settings.isProxyConfigured())
                msg.append(" ").append(tr("STR_VERIFY_PROXY"));            
            return msg;
        }
        else if (certificateStatus == PTEID_CERTIF_STATUS_ERROR
                 || certificateStatus == PTEID_CERTIF_STATUS_ISSUER
                 || certificateStatus == PTEID_CERTIF_STATUS_UNKNOWN)
            return QString(tr("STR_CARD_VALIDATION_ERROR"));
        else if (certificateStatus == PTEID_CERTIF_STATUS_SUSPENDED
                 || certificateStatus == PTEID_CERTIF_STATUS_REVOKED)
            return QString(tr("STR_CARD_CANCELED"));
        else if(certificateStatus == PTEID_CERTIF_STATUS_EXPIRED)
            return QString(tr("STR_CARD_EXPIRED_CERT"));
    }
    //Else If state active AND validity expired
    else if (card->isActive() && isExpiredDate(eid_file.getValidityEndDate()))
    {
        return QString(tr("STR_CARD_EXPIRED"));
    }
    //Else If state not active
    else if (!card->isActive())
    {
        return QString(tr("STR_CARD_NOT_ACTIVE"));
    }
    END_TRY_CATCH

    return QString(tr("STR_CARD_STATUS_FAIL"));
}

QPixmap PhotoImageProvider::requestPixmap(const QString &id, QSize *size, const QSize &requestedSize)
{
    if (id != "photo.png") {
        qDebug() << "PhotoImageProvider: wrong id requested - " << id;
        return QPixmap();
    }

    if (!requestedSize.isValid())
    {
        if (requestedSize.height() > p.height() || requestedSize.width() > p.width())
        {
            qDebug() << "PhotoImageProvider: Invalid requestedSize - " << requestedSize;
            return QPixmap();
        }
    }

    size->setWidth(p.width());
    size->setHeight(p.height());

    return p;
}

void GAPI::startPrintPDF(QString outputFile, bool isBasicInfo, bool isAddicionalInfo,
    bool isAddress, bool isNotes, bool isPrintDate, bool isSign) {

    PrintParams params = { outputFile, isBasicInfo, isAddicionalInfo, isAddress, isNotes, isPrintDate, isSign };
    Concurrent::run(this, &GAPI::doPrintPDF, params);
}

void GAPI::startPrint(QString outputFile, bool isBasicInfo, bool isAddicionalInfo,
    bool isAddress, bool isNotes, bool isPrintDate, bool isSign) {

    if (QPrinterInfo::availablePrinters().size() == 0) {
        qDebug() << "No Printers Available";
        emit signalPrinterPrintFail(NoPrinterAvailable);
        return;
    }

    PrintParams params = { outputFile, isBasicInfo, isAddicionalInfo, isAddress, isNotes, isPrintDate, isSign };

    QPrinter printer;
    bool res = false;

    printer.setDocName("CartaoCidadao.pdf");
    QPrintDialog *dlg = new QPrintDialog(&printer);
    if (dlg->exec() == QDialog::Accepted) {
        qDebug() << "QPrintDialog! Accepted";
        BEGIN_TRY_CATCH;
        // Print PDF not Signed
        res = drawpdf(printer, params);

        if (params.outputFile.toUtf8().size() > 0){
            qDebug() << "Create PDF";
            if (res) {
                emit signalPdfPrintSucess();
             }else{
                emit signalPdfPrintFail();
            }
         }else{
            qDebug() << "Printing to a printer";
            if (res) {
                emit signalPrinterPrintSucess();
             }else{
                emit signalPrinterPrintFail();
            }
        }
        END_TRY_CATCH
    }
}

bool GAPI::doSignPrintPDF(QString &file_to_sign, QString &outputsign) {
	PTEID_LOG(eIDMW::PTEID_LOG_LEVEL_DEBUG, "eidgui", "GetCardInstance doSignPrintPDF");
    BEGIN_TRY_CATCH

        PTEID_EIDCard * card = NULL;

    getCardInstance(card);
    if (card == NULL) return false;

    PTEID_PDFSignature sig_handler(getPlatformNativeString(file_to_sign));

    card->SignPDF(sig_handler, 0, 0, false, "", "", getPlatformNativeString(outputsign));

    return true;

    END_TRY_CATCH

        return false;
}

void GAPI::doPrintPDF(PrintParams &params) {

    qDebug() << "doPrintPDF! outputFile = " << params.outputFile <<
        "isBasicInfo = " << params.isBasicInfo << "isAddicionalInfo" << params.isAddicionalInfo << "isAddress"
        << params.isAddress << "isNotes = " << params.isNotes << "isPrintDate = " << params.isPrintDate << "isSign = " << params.isSign;

    QString pdffiletmp;
    QPrinter pdf_printer;
    QString nativepdftmp;
    QString originalOutputFile;
    bool res = false;

    BEGIN_TRY_CATCH;
    if (params.isSign)
    {
        // Print PDF Signed
        pdffiletmp = QDir::tempPath();
        pdffiletmp.append("/CartaoCidadao.pdf");
        nativepdftmp = QDir::toNativeSeparators(pdffiletmp);
        originalOutputFile = params.outputFile;
        params.outputFile = nativepdftmp;
        res = drawpdf(pdf_printer, params);
        if (!res)
        {
            emit signalPdfPrintFail();
            return;
        }

        QFuture<bool> new_thread = Concurrent::run(this, &GAPI::doSignPrintPDF, nativepdftmp,
            originalOutputFile);

        res = new_thread.result();
        // Emit signal if success but if false a popup about some error is already sent
        if (res)
            emit signalPdfPrintSignSucess();
    } else {
        // Print PDF not Signed
        res = drawpdf(pdf_printer, params);
        if (res) {
            emit signalPdfPrintSucess();
        }else{
            emit signalPdfPrintFail();
        }
    }
    END_TRY_CATCH
}

static QPen black_pen;
static QPen blue_pen;

double GAPI::drawSingleField(QPainter &painter, double pos_x, double pos_y, QString name, QString value, double line_length, int field_margin, bool is_bounded_rect, double bound_width)
{
    painter.setPen(blue_pen);

    // apply scale factor to default values
    if (field_margin == 15)
        field_margin *= print_scale_factor;
    if (bound_width == 360)
        bound_width *= print_scale_factor;
    if (field_margin == 0){
        line_length -= 15 * print_scale_factor;
    }

    painter.drawText(QPointF(pos_x + field_margin, pos_y), name);
    pos_y += 7 * print_scale_factor;

    painter.drawLine(QPointF(pos_x + field_margin, pos_y), QPointF(pos_x + (line_length - field_margin), pos_y));
    painter.setPen(black_pen);

    if (is_bounded_rect){
        int flags = Qt::TextWordWrap | Qt::TextWrapAnywhere;
        int textFlags = Qt::TextWordWrap;
        QFontMetricsF fm = painter.fontMetrics();
        QRectF bounding_rect = fm.boundingRect(QRectF(pos_x + field_margin, pos_y, bound_width - 2 * field_margin, 500 * print_scale_factor), flags, value);

        painter.drawText(bounding_rect.adjusted(0, 0, 0, 0), textFlags, value);
        pos_y += bounding_rect.height() + 15 * print_scale_factor;
    } else {
        pos_y += 15 * print_scale_factor;
        painter.drawText(QPointF(pos_x + field_margin, pos_y), value);
    }

    return pos_y;
}

void GAPI::drawSectionHeader(QPainter &painter, double pos_x, double pos_y, QString section_name)
{
    QFont header_font("DIN Light");
    header_font.setPointSize(11);
    QFont regular_font("DIN Medium");
    regular_font.setPointSize(12);

    painter.setFont(header_font);

    QColor light_grey(233, 233, 233);
    painter.setBrush(light_grey);
    painter.setPen(light_grey);

    painter.drawRoundedRect(QRectF(pos_x, pos_y, 250 * print_scale_factor, 30 * print_scale_factor), 10.0 * print_scale_factor, 10.0 * print_scale_factor);
    painter.setPen(black_pen);

    painter.drawText(pos_x + 20 * print_scale_factor, pos_y + 20 * print_scale_factor, section_name);

    painter.setFont(regular_font);
}

void GAPI::drawPrintingDate(QPainter &painter, QString printing_date){
    QFont date_font("DIN Medium");
    date_font.setPointSize(8);
    date_font.setBold(false);
    QFont regular_font("DIN Medium");
    regular_font.setPointSize(12);

    printing_date += " " + QDateTime::currentDateTime().toString("dd.MM.yyyy hh:mm");
    painter.setFont(date_font);
    painter.drawText(QRectF(painter.device()->width() - 225 * print_scale_factor,
                            painter.device()->height() - 25 * print_scale_factor,
                            200 * print_scale_factor,
                            100 * print_scale_factor),
                    Qt::TextWordWrap, printing_date);

    painter.setFont(regular_font);
}

QPixmap loadHeader()
{
    return QPixmap(":/images/pdf_document_header.png");
}

QString getTextFromLines(QStringList lines, int start, int stop){
    QStringList text_lines;

    //make sure doesn't access a invalid index
    if (stop > lines.length())
    {
        stop = lines.length();
    }

    for (int i = start; i < stop; i++)
    {
        text_lines.append(lines.at(i));
    }

    return text_lines.join("\n");
}

double GAPI::checkNewPageAndPrint(QPrinter &printer, QPainter &painter, double current_y, double remaining_height, double max_height, bool print_date, QString date_label){
    if (current_y + remaining_height > max_height){
        printer.newPage();
        current_y = 50 * print_scale_factor;
        if (print_date)
        {
            drawPrintingDate(painter, date_label);
        }
    }
    return current_y;
}

bool GAPI::drawpdf(QPrinter &printer, PrintParams params)
{
    qDebug() << "drawpdf! outputFile = " << params.outputFile <<
        "isBasicInfo = " << params.isBasicInfo << "isAddicionalInfo" << params.isAddicionalInfo << "isAddress"
        << params.isAddress << "isNotes = " << params.isNotes << "isPrintDate = " << params.isPrintDate << "isSign = " << params.isSign;

    PTEID_LOG(eIDMW::PTEID_LOG_LEVEL_DEBUG, "eidgui", "GetCardInstance drawpdf");

    BEGIN_TRY_CATCH;

    PTEID_EIDCard * card = NULL;
    getCardInstance(card);
    if (card == NULL) return false;

    card->doSODCheck(true); //Enable SOD checking
    PTEID_EId &eid_file = card->getID();

    //QPrinter printer;
    print_scale_factor = printer.resolution() / 96.0;

    printer.setColorMode(QPrinter::Color);
    printer.setPaperSize(QPrinter::A4);

    if (params.outputFile.toUtf8().size() > 0){
        printer.setOutputFileName(params.outputFile.toUtf8().data());
        qDebug() << "Printing PDF";
    }else{
        qDebug() << "Printing to default printer";
    }

    //TODO: Add custom fonts
    //addFonts();

    //Start drawing
    QPainter painter;
    bool res = false;
    res = painter.begin(&printer);
    if (res == false){
        qDebug() << "Start drawing return error: " << res;
        return false;
    }

    //gives a bit of left margin
    double page_margin = 33.5 * print_scale_factor;
    double pos_x = page_margin, pos_y = 0;
    int field_margin = 15 * print_scale_factor;

    double page_height = painter.device()->height();
    double page_width = painter.device()->width();
    double full_page = (page_width - 2 * page_margin);
    double half_page = full_page / 2;
    double third_of_page = (page_width - 2 * page_margin) / 3;

    //Font setup
    QFont din_font("DIN Medium");
    din_font.setPixelSize(18 * print_scale_factor);

    //  Include header as png pixmap
    QPixmap header = loadHeader();
    QRectF headerImgRect = QRectF(pos_x, pos_y, 359.0 * print_scale_factor, 75.0 * print_scale_factor);
    painter.drawPixmap(headerImgRect, header, QRect(0.0, 0.0, header.width(), header.height()));

    //  //Alternative using the QtSVG module, not enabled for now because the rendering is far from perfect
    //QSvgRenderer renderer(QString("C:\\Users\\agrr\\Desktop\\GMC_logo.svg"));
    //std::cout << renderer.defaultSize().width() << "x" << renderer.defaultSize().height() << std::endl;

    //renderer.render(&painter, QRect(pos_x, pos_y, 504, 132));
    //renderer.render(&painter, QRect(pos_x, pos_y, 250, 120));

    pos_y += headerImgRect.height() + field_margin;
    black_pen = painter.pen();

    blue_pen.setColor(QColor(78, 138, 190));
    painter.setPen(blue_pen);

    int line_length = 487 * print_scale_factor;
    //Horizontal separator below the CC logo
    painter.drawLine(QPointF(pos_x, pos_y), QPointF(pos_x + line_length, pos_y));
    pos_y += 25 * print_scale_factor;

    //Change text color
    blue_pen = painter.pen();

    const double LINE_HEIGHT = 45.0 * print_scale_factor;

    //    din_font.setBold(true);
    painter.setFont(din_font);

    painter.drawText(QPointF(pos_x, pos_y), tr("STR_PERSONAL_DATA"));

    pos_y += field_margin;
    int circle_radius = 6 * print_scale_factor;

    painter.setPen(black_pen);
    //Reset font
    din_font.setPixelSize(10 * print_scale_factor);
    din_font.setBold(false);
    painter.setFont(din_font);

    pos_y += 30 * print_scale_factor;

    if (params.isPrintDate)
    {
        drawPrintingDate(painter, tr("STR_PRINTED_ON"));
    }

    if (params.isBasicInfo)
    {

        drawSectionHeader(painter, pos_x, pos_y, tr("STR_BASIC_INFORMATION"));

        //Load photo into a QPixmap
        PTEID_ByteArray& photo = eid_file.getPhotoObj().getphoto();

        //Image
        QPixmap pixmap_photo;
        pixmap_photo.loadFromData(photo.GetBytes(), photo.Size(), "PNG");

        const int img_height = 200 * print_scale_factor;

        //Scale height if needed
        QPixmap scaled = pixmap_photo.scaledToHeight(img_height, Qt::SmoothTransformation);

        painter.drawPixmap(QPointF(pos_x + 500 * print_scale_factor, pos_y - 80 * print_scale_factor), scaled);

        pos_y += 50 * print_scale_factor;

        double new_pos_y = drawSingleField(painter, pos_x, pos_y, tr("STR_GIVEN_NAME"),
            QString::fromUtf8(eid_file.getGivenName()), half_page, 0, true, half_page);
        pos_y = (std::max)(new_pos_y, pos_y + LINE_HEIGHT);

        new_pos_y = drawSingleField(painter, pos_x, pos_y, tr("STR_SURNAME"),
            QString::fromUtf8(eid_file.getSurname()), half_page, 0, true, half_page);
        pos_y = (std::max)(new_pos_y, pos_y + LINE_HEIGHT);

        double f_col = half_page / 2;
        drawSingleField(painter, pos_x, pos_y, tr("STR_GENDER"), QString::fromUtf8(eid_file.getGender()), f_col, 0);
        drawSingleField(painter, pos_x + f_col, pos_y, tr("STR_HEIGHT"), QString::fromUtf8(eid_file.getHeight()), f_col);
        drawSingleField(painter, pos_x + 2 * f_col, pos_y, tr("STR_NATIONALITY"),
            QString::fromUtf8(eid_file.getNationality()), f_col);
        drawSingleField(painter, pos_x + 3 * f_col, pos_y, tr("STR_DATE_OF_BIRTH"),
            QString::fromUtf8(eid_file.getDateOfBirth()), f_col);
        pos_y += LINE_HEIGHT;

        drawSingleField(painter, pos_x, pos_y, tr("STR_DOCUMENT_NUMBER"), QString::fromUtf8(eid_file.getDocumentNumber()), half_page, 0);
        drawSingleField(painter, pos_x + half_page, pos_y, tr("STR_VALIDITY_DATE"),
            QString::fromUtf8(eid_file.getValidityEndDate()), half_page);
        pos_y += LINE_HEIGHT;

        double new_height_left = drawSingleField(painter, pos_x, pos_y, tr("STR_FATHER"),
            QString::fromUtf8(eid_file.getGivenNameFather()) + " " +
            QString::fromUtf8(eid_file.getSurnameFather()), half_page, 0, true, half_page);
        double new_height_right = drawSingleField(painter, pos_x + half_page, pos_y, tr("STR_MOTHER"),
            QString::fromUtf8(eid_file.getGivenNameMother()) + " " +
            QString::fromUtf8(eid_file.getSurnameMother()), half_page, field_margin, true, half_page);
        pos_y = (std::max)((std::max)(new_height_left, new_height_right), pos_y + LINE_HEIGHT);

        new_pos_y = drawSingleField(painter, pos_x, pos_y, tr("STR_NOTES"),
            QString::fromUtf8(eid_file.getAccidentalIndications()), half_page, 0, true, page_width);

        pos_y = (std::max)(new_pos_y, pos_y + LINE_HEIGHT);
    }

    if (params.isAddicionalInfo)
    {
        //height of this section, if spacing between fields change update this value
        double min_section_height = 235 * print_scale_factor;
        pos_y = checkNewPageAndPrint(printer, painter, pos_y, min_section_height, page_height, params.isPrintDate, tr("STR_PRINTED_ON"));

        drawSectionHeader(painter, pos_x, pos_y, tr("STR_ADDITIONAL_INFORMATION"));
        pos_y += 50 * print_scale_factor;

        drawSingleField(painter, pos_x, pos_y, tr("STR_VAT_NUM"),
            QString::fromUtf8(eid_file.getTaxNo()), third_of_page, 0);
        drawSingleField(painter, pos_x + third_of_page, pos_y, tr("STR_SOCIAL_SECURITY_NUM"),
            QString::fromUtf8(eid_file.getSocialSecurityNumber()), third_of_page);
        drawSingleField(painter, pos_x + 2 * third_of_page, pos_y, tr("STR_NATIONAL_HEALTH_NUM"),
            QString::fromUtf8(eid_file.getHealthNumber()), third_of_page);
        pos_y += LINE_HEIGHT;

        drawSingleField(painter, pos_x, pos_y, tr("STR_DELIVERY_ENTITY"),
            QString::fromUtf8(eid_file.getIssuingEntity()), half_page, 0, true, half_page);
        drawSingleField(painter, pos_x + half_page, pos_y, tr("STR_DELIVERY_DATE"),
            QString::fromUtf8(eid_file.getValidityBeginDate()), half_page);
        pos_y += LINE_HEIGHT;

        drawSingleField(painter, pos_x, pos_y, tr("STR_DOCUMENT_TYPE"),
            QString::fromUtf8(eid_file.getDocumentType()), half_page, 0, true, half_page);
        drawSingleField(painter, pos_x + half_page, pos_y, tr("STR_DELIVERY_LOCATION"),
            QString::fromUtf8(eid_file.getLocalofRequest()), half_page, field_margin, true, half_page);
        pos_y += LINE_HEIGHT;

        drawSingleField(painter, pos_x, pos_y, tr("STR_CARD_VERSION"),
            QString::fromUtf8(eid_file.getDocumentVersion()), half_page, 0);
        drawSingleField(painter, pos_x + half_page, pos_y, tr("STR_CARD_STATE"),
            getCardActivation(), half_page, field_margin, true, half_page);
        pos_y += 50 * print_scale_factor;
    }

    if (params.isAddress)
    {
        //height of this section, if spacing between fields change update this value
        double min_section_height = 350 * print_scale_factor;
        pos_y = checkNewPageAndPrint(printer, painter, pos_y, min_section_height, page_height, params.isPrintDate, tr("STR_PRINTED_ON"));

        drawSectionHeader(painter, pos_x, pos_y, tr("STR_ADDRESS"));
        pos_y += 50 * print_scale_factor;

        PTEID_Address &addressFile = card->getAddr();

        if (addressFile.isNationalAddress()){
            double new_height_left = drawSingleField(painter, pos_x, pos_y, tr("STR_DISTRICT"),
                QString::fromUtf8(addressFile.getDistrict()), half_page, 0, true, half_page);
            double new_height_right = drawSingleField(painter, pos_x + half_page, pos_y, tr("STR_MUNICIPALITY"),
                QString::fromUtf8(addressFile.getMunicipality()), half_page,
                field_margin, true, half_page);
            pos_y = (std::max)((std::max)(new_height_left, new_height_right), pos_y + LINE_HEIGHT);

            new_height_left = drawSingleField(painter, pos_x, pos_y, tr("STR_CIVIL_PARISH"),
                QString::fromUtf8(addressFile.getCivilParish()), full_page, 0, true, page_width);
            pos_y = (std::max)(new_height_left, pos_y + LINE_HEIGHT);

            drawSingleField(painter, pos_x, pos_y, tr("STR_STREET_TYPE"),
                QString::fromUtf8(addressFile.getStreetType()), full_page, 0, true, page_width);
            pos_y = (std::max)(new_height_left, pos_y + LINE_HEIGHT);

            new_height_left = drawSingleField(painter, pos_x, pos_y, tr("STR_STREET_NAME"),
                QString::fromUtf8(addressFile.getStreetName()), full_page, 0, true, page_width);
            pos_y = (std::max)(new_height_left, pos_y + LINE_HEIGHT);

            new_height_left = drawSingleField(painter, pos_x, pos_y, tr("STR_HOUSE_BUILDING_NUM"),
                QString::fromUtf8(addressFile.getDoorNo()), third_of_page, 0);
            new_height_right = drawSingleField(painter, pos_x + third_of_page, pos_y, tr("STR_FLOOR"),
                QString::fromUtf8(addressFile.getFloor()), third_of_page);
            double new_height = drawSingleField(painter, pos_x + 2 * third_of_page, pos_y, tr("STR_SIDE"),
                QString::fromUtf8(addressFile.getSide()), third_of_page);
            pos_y = (std::max)(new_height, (std::max)(new_height_left, (std::max)(new_height_left, pos_y + LINE_HEIGHT)));

            drawSingleField(painter, pos_x, pos_y, tr("STR_PLACE"),
                QString::fromUtf8(addressFile.getPlace()), half_page, 0, true, half_page);
            new_height_left = drawSingleField(painter, pos_x + half_page, pos_y, tr("STR_LOCALITY"),
                QString::fromUtf8(addressFile.getLocality()), half_page, field_margin, true, half_page);

            pos_y = (std::max)(new_height_left, pos_y + LINE_HEIGHT);

            drawSingleField(painter, pos_x, pos_y, tr("STR_ZIP_CODE"),
                QString::fromUtf8(addressFile.getZip4()) + "-" + QString::fromUtf8(addressFile.getZip3()), half_page / 2, 0);
            new_height_left = drawSingleField(painter, pos_x + (half_page / 2), pos_y, tr("STR_POSTAL_LOCALITY"),
                QString::fromUtf8(addressFile.getPostalLocality()),
                (3 * half_page) / 2, field_margin, true, (3 * half_page) / 2);
            pos_y = (std::max)(new_height_left, pos_y + LINE_HEIGHT);

        }else{
            /* Foreign Address*/
            drawSingleField(painter, pos_x, pos_y, tr("STR_FOREIGN_COUNTRY"),
                QString::fromUtf8(addressFile.getForeignCountry()), third_of_page, 0);
            drawSingleField(painter, pos_x + third_of_page, pos_y, tr("STR_FOREIGN_REGION"),
                QString::fromUtf8(addressFile.getForeignRegion()), third_of_page);
            drawSingleField(painter, pos_x + 2 * third_of_page, pos_y, tr("STR_FOREIGN_CITY"),
                QString::fromUtf8(addressFile.getForeignCity()), third_of_page);

            pos_y += LINE_HEIGHT;

            drawSingleField(painter, pos_x, pos_y, tr("STR_FOREIGN_LOCALITY"),
                QString::fromUtf8(addressFile.getForeignLocality()), half_page, 0, true, half_page);
            drawSingleField(painter, pos_x + half_page, pos_y, tr("STR_FOREIGN_POSTAL_CODE"),
                QString::fromUtf8(addressFile.getForeignPostalCode()), half_page, field_margin, true, half_page);

            pos_y += LINE_HEIGHT;

            drawSingleField(painter, pos_x, pos_y, tr("STR_FOREIGN_ADDRESS"),
                QString::fromUtf8(addressFile.getForeignAddress()), half_page, 0, true, page_width);
        }
        pos_y += 80 * print_scale_factor;
    }

    if (params.isNotes)
    {
        QString perso_data;

        perso_data = QString(card->readPersonalNotes());

        if (perso_data.size() > 0)
        {
            pos_x = 30 * print_scale_factor; //gives a bit of left margin
            pos_y += 50 * print_scale_factor;

            pos_y = checkNewPageAndPrint(printer, painter, pos_y, 0, page_height, params.isPrintDate, tr("STR_PRINTED_ON"));

            drawSectionHeader(painter, pos_x, pos_y, tr("STR_PERSONAL_NOTES"));

            pos_y += 50 * print_scale_factor;

            QStringList lines = perso_data.split("\n", QString::KeepEmptyParts);

            const int TEXT_LINE_HEIGHT = 20 * print_scale_factor;

            int line_count = lines.length();
            double height_to_print = TEXT_LINE_HEIGHT * line_count;
            int line_index_start = 0;
            int line_index_stop = 0;
            int completed_lines = 0;
            double notes_width = painter.device()->width() - 2 * page_margin;

            QFontMetrics fontMetrics(din_font);
            while (completed_lines < line_count){
                int page_remaining_space = static_cast<int>(page_height - pos_y);

                line_index_stop = line_index_start + (page_remaining_space / TEXT_LINE_HEIGHT);

                if (page_remaining_space < 50 * print_scale_factor && height_to_print > 0)
                {
                    printer.newPage();
                    pos_y = 50 * print_scale_factor;
                    drawSectionHeader(painter, pos_x, pos_y, tr("STR_PERSONAL_NOTES"));
                    pos_y += 50 * print_scale_factor;
                    if (params.isPrintDate)
                    {
                        drawPrintingDate(painter, tr("STR_PRINTED_ON"));
                    }
                    // not enough area recalculate lines needed
                    page_remaining_space = static_cast<int>(page_height - pos_y);
                    line_index_stop = line_index_start + (page_remaining_space / TEXT_LINE_HEIGHT);
                }

                QString text = getTextFromLines(lines, line_index_start, line_index_stop);

                double diff = line_index_stop - line_index_start;
                double height = diff * TEXT_LINE_HEIGHT;

                QRectF bounding_rect = fontMetrics.boundingRect(text);
                QRectF textRect(pos_x, pos_y, notes_width, height);
                painter.drawText(textRect, Qt::TextWrapAnywhere, text, &bounding_rect);
                height_to_print -= height;
                pos_y += height;
                completed_lines += diff;
                line_index_start = line_index_stop;
            }
        }
    }
    //Finish drawing/printing
    painter.end();
    END_TRY_CATCH
        return true;
}

void GAPI::startSigningPDF(QString loadedFilePath, QString outputFile, int page, double coord_x, double coord_y,
    QString reason, QString location, bool isTimestamp, bool isLTV, bool isSmall) {
    //TODO: refactor startSigningPDF/startSigningBatchPDF                           
    QList<QString> loadedFilePaths = { loadedFilePath };
    SignParams params = { loadedFilePaths, outputFile, page, coord_x, coord_y, reason, location, isTimestamp, isLTV, isSmall };
    QFuture<void> future =
        Concurrent::run(this, &GAPI::doSignPDF, params);

}

void GAPI::startSigningBatchPDF(QList<QString> loadedFileBatchPath, QString outputFile, int page, double coord_x, double coord_y,
    QString reason, QString location, bool isTimestamp, bool isLTV, bool isSmall) {

    SignParams params = { loadedFileBatchPath, outputFile, page, coord_x, coord_y, reason, location, isTimestamp, isLTV, isSmall };

    QFuture<void> future =
        Concurrent::run(this, &GAPI::doSignBatchPDF, params);
}

int GAPI::getPDFpageCount(QString loadedFilePath) {

    PTEID_PDFSignature sig_handler(getPlatformNativeString(loadedFilePath));

    int pageCount = sig_handler.getPageCount();

    return pageCount;
}

void GAPI::closePdfPreview(QString filePath)
{
    image_provider_pdf->closeDoc(filePath);
}
void GAPI::closeAllPdfPreviews() 
{
    qDebug() << "closeAllPdfPreviews";
    image_provider_pdf->closeAllDocs();
}

void GAPI::startSigningXADES(QString loadedFilePath, QString outputFile, bool isTimestamp) {
    QFuture<void> future =
        Concurrent::run(this, &GAPI::doSignXADES, loadedFilePath, outputFile, isTimestamp);

}

void GAPI::startSigningBatchXADES(QList<QString> loadedFileBatchPath, QString outputFile, bool isTimestamp) {

    SignParams params = { loadedFileBatchPath, outputFile, 0, 0, 0, "", "", isTimestamp, false, 0 };

    QFuture<void> future =
        Concurrent::run(this, &GAPI::doSignBatchXADES, params);
}

void GAPI::doSignBatchXADES(SignParams &params) {
    qDebug() << "doSignBatchXADES! loadedFilePath = " << params.loadedFilePaths << " outputFile = " << params.outputFile <<
        "page = " << params.page << "coord_x" << params.coord_x << "coord_y" << params.coord_y <<
        "reason = " << params.reason << "location = " << params.location << "isTimestamp = " << params.isTimestamp;

    PTEID_LOG(eIDMW::PTEID_LOG_LEVEL_DEBUG, "eidgui", "GetCardInstance doSignBatchXADES");

    BEGIN_TRY_CATCH

        PTEID_EIDCard * card = NULL;
    getCardInstance(card);
    if (card == NULL) return;

    QVector<const char *> files_to_sign;

    for (int i = 0; i < params.loadedFilePaths.count(); i++){
        files_to_sign.push_back(strdup(getPlatformNativeString(params.loadedFilePaths[i])));
    }

    QByteArray tempOutputFile = getPlatformNativeString(params.outputFile);

    if (params.isTimestamp)
        card->SignXadesT(tempOutputFile.constData(), files_to_sign.data(), params.loadedFilePaths.count());
    else
        card->SignXades(tempOutputFile.constData(), files_to_sign.data(), params.loadedFilePaths.count());

    emit signalPdfSignSucess(SignMessageOK);

    END_TRY_CATCH
}

bool GAPI::isDirectory(QString path) {
    QFileInfo fi(path);
    return fi.isDir();
}

bool GAPI::isFile(QString path) {
    QFileInfo fi(path);
    return fi.isFile();
}

// Checks if the given file status or path corresponds to an existing file or directory.
bool GAPI::fileExists(QString path) {
    QFileInfo check_file(path);
    return check_file.exists();
}

QList<QString> GAPI::getFilesFromDirectory(QString path) {
    QList<QString> allFiles;
    if (isDirectory(path)) {
        QDir dir(path);
        if (dir.exists() && !dir.isEmpty()) {
            QFileInfoList list = dir.entryInfoList(QDir::Files | QDir::NoSymLinks | QDir::NoDotAndDotDot | QDir::Readable);
            for (int i = 0; i < list.size(); ++i) {
                QFileInfo fi = list.at(i);
                allFiles.append(fi.absoluteFilePath());
            }
        }
    }
    qDebug() << "QSTring files " << allFiles;
    return allFiles;
}

void GAPI::doSignXADES(QString loadedFilePath, QString outputFile, bool isTimestamp) {
    PTEID_LOG(eIDMW::PTEID_LOG_LEVEL_DEBUG, "eidgui", "GetCardInstance doSignXADES");

    BEGIN_TRY_CATCH

        PTEID_EIDCard * card = NULL;
    getCardInstance(card);
    if (card == NULL) return;

    const char *files_to_sign[1];
    QByteArray tempLoadedFilePath = getPlatformNativeString(loadedFilePath);
    files_to_sign[0] = tempLoadedFilePath.constData();

    QByteArray tempOutputFile = getPlatformNativeString(outputFile);

    if (isTimestamp)
        card->SignXadesT(tempOutputFile.constData(), files_to_sign, 1);
    else
        card->SignXades(tempOutputFile.constData(), files_to_sign, 1);

    emit signalPdfSignSucess(SignMessageOK);

    END_TRY_CATCH
}

void GAPI::doSignPDF(SignParams &params) {

    PTEID_LOG(eIDMW::PTEID_LOG_LEVEL_DEBUG, "eidgui", "GetCardInstance doSignPDF");

    BEGIN_TRY_CATCH

        PTEID_EIDCard * card = NULL;
    getCardInstance(card);
    if (card == NULL) return;

    QString fullInputPath = params.loadedFilePaths[0];
    PTEID_PDFSignature sig_handler(getPlatformNativeString(fullInputPath));

    if (params.isTimestamp)
    {
        if (params.isLtv)
        {
            sig_handler.setSignatureLevel(PTEID_LEVEL_LTV);
        }
        else
        {
            sig_handler.setSignatureLevel(PTEID_LEVEL_TIMESTAMP);
        }
    }

    if (params.isSmallSignature) {
        sig_handler.enableSmallSignatureFormat();
    }

    if (useCustomSignature()) {
        const PTEID_ByteArray imageData(reinterpret_cast<const unsigned char *>(m_jpeg_scaled_data.data()), static_cast<unsigned long>(m_jpeg_scaled_data.size()));
        sig_handler.setCustomImage(imageData);
    }
    card->SignPDF(sig_handler, params.page, params.coord_x, params.coord_y,
        params.location.toUtf8().data(), params.reason.toUtf8().data(),
        getPlatformNativeString(params.outputFile));

    emit signalPdfSignSucess(SignMessageOK);

    END_TRY_CATCH
}

void GAPI::doSignBatchPDF(SignParams &params) {

    qDebug() << "doSignBatchPDF! loadedFilePath = " << params.loadedFilePaths << " outputFile = " << params.outputFile <<
        "page = " << params.page << "coord_x" << params.coord_x << "coord_y" << params.coord_y <<
        "reason = " << params.reason << "location = " << params.location;

    PTEID_LOG(eIDMW::PTEID_LOG_LEVEL_DEBUG, "eidgui", "GetCardInstance doSignBatchPDF");

    BEGIN_TRY_CATCH

        PTEID_EIDCard * card = NULL;
    getCardInstance(card);
    if (card == NULL) return;

    PTEID_PDFSignature *sig_handler;
    sig_handler = new PTEID_PDFSignature();

    for (int i = 0; i < params.loadedFilePaths.count(); i++){
        qDebug() << params.loadedFilePaths[i];
        QString fullInputPath = params.loadedFilePaths[i];
        sig_handler->addToBatchSigning((char *)getPlatformNativeString(fullInputPath),
            params.page == 0 ? true : false);
    }

    if (params.isTimestamp)
    {
        if (params.isLtv)
        {
            sig_handler->setSignatureLevel(PTEID_LEVEL_LTV);
        }
        else
        {
            sig_handler->setSignatureLevel(PTEID_LEVEL_TIMESTAMP);
        }
    }

    if (params.isSmallSignature) {
        sig_handler->enableSmallSignatureFormat();
    }

    if (useCustomSignature()) {
        const PTEID_ByteArray imageData(reinterpret_cast<const unsigned char *>(m_jpeg_scaled_data.data()), static_cast<unsigned long>(m_jpeg_scaled_data.size()));
        sig_handler->setCustomImage(imageData);
    }

    card->SignPDF(*sig_handler, params.page, params.coord_x, params.coord_y,
        params.location.toUtf8().data(), params.reason.toUtf8().data(),
        getPlatformNativeString(params.outputFile));

    emit signalPdfSignSucess(SignMessageOK);

    END_TRY_CATCH
}

QPixmap PDFPreviewImageProvider::requestPixmap(const QString &id, QSize *size, const QSize &requestedSize)
{
    /*qDebug() << "PDFPreviewImageProvider received request for: " << id;*/
    qDebug() << "PDFPreviewImageProvider received request for (width height): "
        << requestedSize.width() << " - " << requestedSize.height();
    QStringList strList = id.split("?");

    QString pdf_path = QUrl::fromPercentEncoding(strList.at(0).toUtf8());
    m_filePath = pdf_path.toStdString();

    //URL param ?page=xx
    unsigned int page = (unsigned int)strList.at(1).split("=").at(1).toInt();

    if (m_docs.find(m_filePath) == m_docs.end()) {

        Poppler::Document *newdoc = Poppler::Document::load(pdf_path);
        if (!newdoc) {
            qDebug() << "Failed to load PDF file";
            return QPixmap();
        }
        m_docs.insert(std::pair<std::string, Poppler::Document *>(m_filePath, newdoc));

        newdoc->setRenderHint(Poppler::Document::TextAntialiasing, true);
        newdoc->setRenderHint(Poppler::Document::Antialiasing, true);
        newdoc->setRenderBackend(Poppler::Document::RenderBackend::SplashBackend);
    }

    QPixmap p = renderPdf(page, requestedSize);
    size->setHeight(p.height());
    size->setWidth(p.width());

    return p;
}

QPixmap PDFPreviewImageProvider::renderPdf(int page, const QSize &requestedSize) {
    QPixmap pTest = renderPDFPage(page);
    qDebug() << "PDFPreviewImageProvider sending signal signalPdfSourceChanged width : "
        << pTest.width() << " - height : " << pTest.height();
    emit signalPdfSourceChanged(pTest.width(), pTest.height());

    QPixmap p = pTest.scaled(requestedSize.width(), requestedSize.height(),
        Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return p;
}

QSize PDFPreviewImageProvider::getPageSize(int page) {
    Poppler::Page *popplerPage = m_docs.at(m_filePath)->page(page - 1);
    return popplerPage->pageSize();
}

QPixmap PDFPreviewImageProvider::renderPDFPage(unsigned int page)
{
    renderMutex.lock();
    if (m_docs.empty())
    {
        qDebug() << "Pdf to be rendered was unloaded!";
        return QPixmap();
    }
    // Document starts at page 0 in the poppler-qt5 API
    Poppler::Page *popplerPage = m_docs.at(m_filePath)->page(page - 1);

    //TODO: Test the resolution on Windows
    const double resX = 120.0;
    const double resY = 120.0;
    if (popplerPage == NULL)
    {
        qDebug() << "Failed to get page object: " << page;
        return QPixmap();
    }

    QImage image = popplerPage->renderToImage(resX, resY, -1, -1, -1, -1, Poppler::Page::Rotate0);
    //DEBUG
    //image.save("/tmp/pteid_preview.png");

    delete popplerPage;
    renderMutex.unlock();

    if (!image.isNull()) {
        return QPixmap::fromImage(image);
    } else {
        qDebug() << "Error rendering PDF page to image!";
        return QPixmap();
    }

}

void PDFPreviewImageProvider::closeDoc(QString filePath) {
    Concurrent::run(this, &PDFPreviewImageProvider::doCloseDoc, filePath);
}
void PDFPreviewImageProvider::closeAllDocs() {
    Concurrent::run(this, &PDFPreviewImageProvider::doCloseAllDocs);
}
void PDFPreviewImageProvider::doCloseDoc(QString filePath) {
    renderMutex.lock();
    std::string filePathStd = filePath.toStdString();
    if (m_docs.find(filePathStd) != m_docs.end())
    {
        delete m_docs.at(filePathStd);
        m_docs.erase(filePathStd);
    }
    renderMutex.unlock();
}
void PDFPreviewImageProvider::doCloseAllDocs() {
    renderMutex.lock();
    std::unordered_map<std::string, Poppler::Document *>::iterator it = m_docs.begin();
    while (it != m_docs.end())
    {
        delete it->second;
        it++;
    }
    m_docs.clear();
    renderMutex.unlock();
}

void GAPI::startCardReading() {
    PTEID_LOG(eIDMW::PTEID_LOG_LEVEL_DEBUG, "eidgui", "StartCardReading connectToCard");
    QFuture<void> future = Concurrent::run(this, &GAPI::connectToCard);

}

void GAPI::startSavingCardPhoto(QString outputFile) {
    QFuture<void> future = Concurrent::run(this, &GAPI::doSaveCardPhoto, outputFile);
}

void GAPI::startWritingPersoNotes(QString text) {
    QFuture<void> future = Concurrent::run(this, &GAPI::setPersoDataFile, text);
}

int GAPI::getStringByteLength(QString text) {

    return text.toStdString().size() + 1; // '\0' should be considered as a byte
}

void GAPI::startGettingEntities() {
    Concurrent::run(this, &GAPI::getSCAPEntities);
}

void GAPI::startGettingCompanyAttributes(bool useOauth) {
    Concurrent::run(this, &GAPI::getSCAPCompanyAttributes, useOauth);
}

void GAPI::startGettingEntityAttributes(QList<int> entities_index, bool useOAuth) {
    Concurrent::run(this, &GAPI::getSCAPEntityAttributes, entities_index, useOAuth);
}

void GAPI::startPingSCAP() {

    // schedule the request
    httpRequestAborted = httpRequestSuccess = false;

    PTEID_LOG(eIDMW::PTEID_LOG_LEVEL_DEBUG, "ScapSignature", "Start Ping SCAP");

    const char * as_endpoint = "/CCC-REST/rest/scap/pingSCAP";

    // Get Endpoint from settings
    ScapSettings settings;
    std::string port = settings.getScapServerPort().toStdString();
    std::string sup_endpoint = std::string("https://")
        + settings.getScapServerHost().toStdString() + ":" + port + as_endpoint;

    url = sup_endpoint.c_str();

    eIDMW::PTEID_Config config_pacfile(eIDMW::PTEID_PARAM_PROXY_PACFILE);
    const char * pacfile_url = config_pacfile.getString();

    if (pacfile_url != NULL && strlen(pacfile_url) > 0)
    {
        m_pac_url = QString(pacfile_url);
    }

    eIDMW::PTEID_Config config(eIDMW::PTEID_PARAM_PROXY_HOST);
    eIDMW::PTEID_Config config_port(eIDMW::PTEID_PARAM_PROXY_PORT);
    eIDMW::PTEID_Config config_username(eIDMW::PTEID_PARAM_PROXY_USERNAME);
    eIDMW::PTEID_Config config_pwd(eIDMW::PTEID_PARAM_PROXY_PWD);

    std::string proxy_host = config.getString();
    std::string proxy_username = config_username.getString();
    std::string proxy_pwd = config_pwd.getString();
    long proxy_port = config_port.getLong();

    //10 second timeout
    int network_timeout = 10000;

    proxy = QNetworkProxy();

    if (!proxy_host.empty() && proxy_port != 0)
    {
        eIDMW::PTEID_LOG(eIDMW::PTEID_LOG_LEVEL_DEBUG, "ScapSignature", "PingSCAP: using manual proxy config");
        qDebug() << "C++: PingSCAP: using manual proxy config";
        proxy.setType(QNetworkProxy::HttpProxy);
        proxy.setHostName(QString::fromStdString(proxy_host));
        proxy.setPort(proxy_port);

        if (!proxy_username.empty())
        {
            proxy.setUser(QString::fromStdString(proxy_username));
            proxy.setPassword(QString::fromStdString(proxy_pwd));
        }
    }
    else if (!m_pac_url.isEmpty())
    {
        std::string proxy_port_str;
        PTEID_LOG(PTEID_LOG_LEVEL_DEBUG, "ScapSignature", "PingSCAP: using system proxy config");
        qDebug() << "C++: PingSCAP: using system proxy config";
        PTEID_GetProxyFromPac(m_pac_url.toUtf8().constData(),
            url.toString().toUtf8().constData(), &proxy_host, &proxy_port_str);
        proxy.setType(QNetworkProxy::HttpProxy);
        proxy.setHostName(QString::fromStdString(proxy_host));
        proxy.setPort(atol(proxy_port_str.c_str()));
    }
    QNetworkProxy::setApplicationProxy(proxy);

    reply = qnam.get(QNetworkRequest(url));

    QTimer::singleShot(network_timeout, this, SLOT(cancelDownload()));
    connect(reply, SIGNAL(finished()),
        this, SLOT(httpFinished()));
}

void GAPI::cancelDownload()
{
    if (!httpRequestSuccess && !httpRequestAborted){
        qDebug() << "C++: signalSCAPPingFail";
        httpRequestAborted = true;
        httpRequestSuccess = false;
        emit signalSCAPPingFail();
        reply->deleteLater();
    }
}

void GAPI::httpFinished()
{
    qDebug() << "C++: httpFinished";
    if (!httpRequestSuccess && !httpRequestAborted){

        if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute) != 200) {
            qDebug() << "C++: reply error";
            httpRequestAborted = true;
            httpRequestSuccess = false;
            emit signalSCAPPingFail();
            QString strLog = QString("PingSCAP: Http request FAIL to: ");
            strLog += reply->url().toString();
            PTEID_LOG(PTEID_LOG_LEVEL_ERROR, "ScapSignature", strLog.toStdString().c_str());
        } else {
            qDebug() << "C++: signalSCAPPingSuccess";
            QString strLog = QString("PingSCAP: Http request SUCCESS to: ");
            strLog += reply->url().toString();
            PTEID_LOG(PTEID_LOG_LEVEL_CRITICAL, "ScapSignature", strLog.toStdString().c_str());
            httpRequestAborted = false;
            httpRequestSuccess = true;
            emit signalSCAPPingSuccess();
        }
        reply->deleteLater();
    }
}

void GAPI::startReadingPersoNotes() {
    QFuture<void> future = Concurrent::run(this, &GAPI::getPersoDataFile);
}

void GAPI::startReadingAddress() {
    Concurrent::run(this, &GAPI::getAddressFile);
}

void GAPI::startLoadingAttributesFromCache(int scapAttrType, bool isShortDescription) {
    Concurrent::run(this, &GAPI::getSCAPAttributesFromCache, scapAttrType, isShortDescription);
}

void GAPI::startRemovingAttributesFromCache(int scapAttrType) {
    Concurrent::run(this, &GAPI::removeSCAPAttributesFromCache, scapAttrType);
}

void GAPI::startSigningSCAP(QString inputPDF, QString outputPDF, int page, double location_x,
    double location_y, QString location, QString reason, bool isTimestamp, bool isLtv,  QList<int> attribute_index) {

    SCAPSignParams signParams = { inputPDF, outputPDF, page, location_x, location_y,
        reason, location, isTimestamp, isLtv, attribute_index };

    Concurrent::run(this, &GAPI::doSignSCAP, signParams);
}

void GAPI::doSignSCAP(SCAPSignParams params) {

    BEGIN_TRY_CATCH

        std::vector<int> attrs;
    for (int i = 0; i != params.attribute_index.size(); i++) {
        attrs.push_back(params.attribute_index.at(i));
    }

    scapServices.executeSCAPSignature(this, params.inputPDF, params.outputPDF, params.page,
        params.location_x, params.location_y, params.location, params.reason,
        params.isTimestamp, params.isLtv, attrs, useCustomSignature(), m_jpeg_scaled_data);
    END_TRY_CATCH
}

void GAPI::getSCAPEntities() {

    QList<QString> attributeSuppliers;
    std::vector<ns3__AttributeSupplierType *> entities = scapServices.getAttributeSuppliers();

    if (entities.size() == 0){
        PTEID_LOG(eIDMW::PTEID_LOG_LEVEL_ERROR, "ScapSignature", "Get SCAP Entities returned zero");
        emit signalSCAPDefinitionsServiceFail(ScapGenericError, false);
        return;
    }

    for (unsigned int i = 0; i != entities.size(); i++)
        attributeSuppliers.append(QString::fromStdString(entities.at(i)->Name));

    emit signalSCAPEntitiesLoaded(attributeSuppliers);
}

bool isSpecialValidity(QString value) {
    QString year = "9999", month = "12", day = "31";
    return value.contains(year) && value.contains(month) && value.contains(day);
}

std::vector<std::string> getChildAttributes(ns2__AttributesType *attributes, bool isShortDescription) {
    std::vector<std::string> childrensList;

    if (attributes->SignedAttributes == NULL){
            return childrensList;
    }
    std::vector<ns5__SignatureType *> signatureAttributeList = attributes->SignedAttributes->ns3__SignatureAttribute;

    for (uint i = 0; i < signatureAttributeList.size(); i++) {
        ns5__SignatureType * signatureType = signatureAttributeList.at(i);
        if (signatureType->ns5__Object.size() > 0)
        {
            ns5__ObjectType * signatureObject = signatureType->ns5__Object.at(0);
            ns3__PersonalDataType * personalDataObject = signatureObject->union_ObjectType.ns3__Attribute->PersonalData;
            ns3__MainAttributeType * mainAttributeObject = signatureObject->union_ObjectType.ns3__Attribute->MainAttribute;
            std::string validity = signatureObject->union_ObjectType.ns3__Attribute->Validity;

            std::string name = personalDataObject->Name;
            childrensList.push_back(name.c_str());

            std::string description = mainAttributeObject->Description->c_str();
			if (!isShortDescription) {

				QString subAttributes(" (");
				QString subAttributesValues;
				uint subAttributePos = 0;

				while (mainAttributeObject->SubAttributeList != NULL && subAttributePos < mainAttributeObject->SubAttributeList->SubAttribute.size()) {
					ns3__SubAttributeType * subAttribute = mainAttributeObject->SubAttributeList->SubAttribute.at(subAttributePos);
					QString subDescription, subValue;
					if (subAttribute->Description != NULL) {
						subDescription += subAttribute->Description->c_str();
					}
					if (subAttribute->Value != NULL) {
						subValue += subAttribute->Value->c_str();
					}
					// Don't append special validity date -> "31 12 9999"
					if (!isSpecialValidity(subValue))
						subAttributesValues.append(subDescription + ": " + subValue + ", ");
					subAttributePos++;
				}
				// Chop 2 to remove last 2 chars (', ')
				subAttributesValues.chop(2);
				subAttributes.append(subAttributesValues + ")");

				/* qDebug() << "Sub attributes : " << subAttributes; */
				if (subAttributes != " ()") // don't append empty parenthesis
					description += subAttributes.toStdString();
			}

            childrensList.push_back(description.c_str());
            childrensList.push_back(validity);
        }
    }
    return childrensList;
}

void GAPI::initScapAppId(){
    ScapSettings settings;
    if (settings.getAppID() == ""){
        // Remove cache from older SCAP implementation
        removeSCAPAttributesFromCache(ScapAttrAll);
        QString appIDstring;
        QString request_uuid = QUuid::createUuid().toString();
        appIDstring = request_uuid.midRef(1, request_uuid.size() - 2).toString();
        settings.setAppID(appIDstring);
    }
}

void GAPI::getSCAPEntityAttributes(QList<int> entityIDs, bool useOAuth) {

    QList<QString> attribute_list;
    PTEID_LOG(eIDMW::PTEID_LOG_LEVEL_DEBUG, "ScapSignature", "GetCardInstance getSCAPEntityAttributes");

    if (!prepareSCAPCache(ScapAttrEntities)){
        return;
    }

    PTEID_EIDCard * card = NULL;
    if (useOAuth){
        emit signalBeginOAuth();
    }
    else {
        getCardInstance(card);
    }
    if (!useOAuth && card == NULL) {
        PTEID_LOG(eIDMW::PTEID_LOG_LEVEL_ERROR, "ScapSignature", "SCAP Entities Attributes Loaded Error!");
        emit signalEntityAttributesLoadedError();
        return;
    }

    initScapAppId();

    std::vector<int> supplier_ids;

    int supplier_id;
    foreach(supplier_id, entityIDs) {
        supplier_ids.push_back(supplier_id);
    }

    std::vector<ns2__AttributesType *> attributes = scapServices.getAttributes(this, card, supplier_ids, useOAuth);

    if (attributes.size() == 0) {
        return;
    }

    getSCAPAttributesFromCache(false,false);
}


void GAPI::getSCAPCompanyAttributes(bool useOAuth) {

    PTEID_LOG(eIDMW::PTEID_LOG_LEVEL_DEBUG, "ScapSignature", "GetCardInstance getSCAPCompanyAttributes");

    if (!prepareSCAPCache(ScapAttrCompanies)) {
        return;
    }

    PTEID_EIDCard * card = NULL;
    QList<QString> attribute_list;
    if (useOAuth){
        emit signalBeginOAuth();
    }
    else {
        getCardInstance(card);
    }
    if (!useOAuth && card == NULL) {
        PTEID_LOG(eIDMW::PTEID_LOG_LEVEL_DEBUG, "ScapSignature", "SCAP Companies Attributes Loaded Error!");
        emit signalCompanyAttributesLoadedError();
        return;
    }

    initScapAppId();

    std::vector<int> supplierIDs;

    std::vector<ns2__AttributesType *> attributes = scapServices.getAttributes(this, card, supplierIDs, useOAuth);

    if (attributes.size() == 0)
    {
        return;
    }

    getSCAPAttributesFromCache(true,false);
}

bool GAPI::isAttributeExpired(std::string& date, std::string& supplier) {
    QRegExp dateFormat("^\\d{4}-\\d{2}-\\d{2}$"); // xsd:date format
    if (date.empty() || dateFormat.indexIn(date.c_str()) == -1) {
        PTEID_LOG(eIDMW::PTEID_LOG_LEVEL_DEBUG, "eidgui",
            "getSCAPAttributesFromCache: Bad date format in validity of attribute supplied by: '%s' -> '%s'", supplier.c_str(), date.c_str());
            return false;
    }

    std::string today;
    const char * format = "%Y-%m-%d"; // xsd:date format
    CTimestampUtil::getTimestamp(today,0L,format);
    bool isExpired = today.compare(date) > 0; // today is after validity -> expired
    if (isExpired) {
        PTEID_LOG(eIDMW::PTEID_LOG_LEVEL_DEBUG, "eidgui",
            "getSCAPAttributesFromCache: Attribute supplied by: '%s' may be expired -> '%s'", supplier.c_str(), date.c_str());
    }

    return isExpired;
}

void GAPI::getSCAPAttributesFromCache(int scapAttrType, bool isShortDescription) {

    qDebug() << "getSCAPAttributesFromCache scapAttrType: " << scapAttrType;

    PTEID_LOG(eIDMW::PTEID_LOG_LEVEL_DEBUG, "ScapSignature", "GetCardInstance getSCAPAttributesFromCache");

    if (!prepareSCAPCache(ScapAttrCompanies)) {
        return;
    }

    std::vector<ns2__AttributesType *> attributes;
    QList<QString> attribute_list;

    //Card is only required for loading entities and companies seperately...
    if (scapAttrType != ScapAttrAll) {
        attributes = scapServices.loadAttributesFromCache(scapAttrType == ScapAttrCompanies);
    }
    // Loading all attributes without NIC
    else {
        attributes = scapServices.reloadAttributesFromCache();
    }

    QStringList possiblyExpiredSuppliers;
    for (uint i = 0; i < attributes.size(); i++) {
        //Skip malformed AttributeResponseValues element
        if (attributes.at(i)->ATTRSupplier == NULL) {
            continue;
        }
        std::string attrSupplier = attributes.at(i)->ATTRSupplier->Name;
        std::vector<std::string> childAttributes = getChildAttributes(attributes.at(i), isShortDescription);

        for (uint j = 0; j < childAttributes.size(); j = j + 3) {
            attribute_list.append(QString::fromStdString(attrSupplier));
            attribute_list.append(QString::fromStdString(childAttributes.at(j)));
            attribute_list.append(QString::fromStdString(childAttributes.at(j + 1)));
            //check validity of attributes
            if (isAttributeExpired(childAttributes.at(j + 2), attrSupplier)) {
                possiblyExpiredSuppliers.push_back(QString::fromStdString(attrSupplier));
            }
        }
    }
    if (scapAttrType == ScapAttrEntities)
        emit signalEntityAttributesLoaded(attribute_list);
    else if (scapAttrType == ScapAttrCompanies)
        emit signalCompanyAttributesLoaded(attribute_list);
    else if (scapAttrType == ScapAttrAll)
        emit signalAttributesLoaded(attribute_list);

    if (!possiblyExpiredSuppliers.empty()) {
        possiblyExpiredSuppliers.removeDuplicates();
        emit signalAttributesPossiblyExpired(possiblyExpiredSuppliers);
    }
}

void GAPI::removeSCAPAttributesFromCache(int scapAttrType) {

    qDebug() << "removeSCAPAttributesFromCache scapAttrType: " << scapAttrType;
    PTEID_LOG(eIDMW::PTEID_LOG_LEVEL_DEBUG, "ScapSignature",
        "Remove SCAP Attributes From Cache scapAttrType = %d", scapAttrType);

    ScapSettings settings;
    QString scapCacheDir = settings.getCacheDir() + "/scap_attributes/";
    QDir dir(scapCacheDir);
    bool has_read_permissions = true;

    // Delete all SCAP secretkey's and SCAP AppID
    settings.resetScapKeys();

#ifdef WIN32
    extern Q_CORE_EXPORT int qt_ntfs_permission_lookup;
    qt_ntfs_permission_lookup++; // turn ntfs checking (allows isReadable and isWritable)
#endif
    if (!dir.isReadable())
    {
        PTEID_LOG(PTEID_LOG_LEVEL_ERROR, "ScapSignature",
            "No read permissions: SCAP cache directory!");
        qDebug() << "C++: Cache folder does not have read permissions! ";
        has_read_permissions = false;
        emit signalCacheNotReadable(scapAttrType);
    }
#ifdef WIN32
    qt_ntfs_permission_lookup--; // turn ntfs permissions lookup off for performance
#endif

    bool status = false;
    status = scapServices.removeAttributesFromCache();

    if (!has_read_permissions)
        return;

    if (status == true)
        emit signalRemoveSCAPAttributesSucess(scapAttrType);
    else
        emit signalRemoveSCAPAttributesFail(scapAttrType);
}

bool GAPI::prepareSCAPCache(int scapAttrType) {
    ScapSettings settings;
    QString s_scapCacheDir = settings.getCacheDir() + "/scap_attributes/";
    QFileInfo scapCacheDir(s_scapCacheDir);
    QDir scapCache(s_scapCacheDir);
    bool hasPermissions = true;
    // Tries to create if does not exist
    if (!scapCache.mkpath(s_scapCacheDir)) {
        qDebug() << "couldn't create SCAP cache folder";
        PTEID_LOG(PTEID_LOG_LEVEL_ERROR, "ScapSignature",
            "Couldn't create SCAP cache folder");
        emit signalCacheFolderNotCreated();
        hasPermissions = false;
    }
#ifdef WIN32
    extern Q_CORE_EXPORT int qt_ntfs_permission_lookup;
    qt_ntfs_permission_lookup++; // turn ntfs checking (allows isReadable and isWritable)
#endif
    if (!scapCacheDir.isWritable()) {
        qDebug() << "SCAP cache not writable";
        PTEID_LOG(PTEID_LOG_LEVEL_ERROR, "ScapSignature",
            "SCAP cache not writable");
        emit signalCacheNotWritable();
        hasPermissions = false;
    }
    if (!scapCacheDir.isReadable()) {
        qDebug() << "SCAP cache not readable";
        PTEID_LOG(PTEID_LOG_LEVEL_ERROR, "ScapSignature",
            "SCAP cache not readable");
        emit signalCacheNotReadable(scapAttrType);
        hasPermissions = false;
    }
#ifdef WIN32
    qt_ntfs_permission_lookup--; // turn ntfs permissions lookup off for performance
#endif
    return hasPermissions;
}

void GAPI::abortSCAPWithCMD() {
    scapServices.cancelGetAttributesWithCMD();
}

void GAPI::getCardInstance(PTEID_EIDCard * &new_card) {

    try
    {
        unsigned long ReaderCount = ReaderSet.readerCount();
        PTEID_LOG(PTEID_LOG_LEVEL_DEBUG, "eidgui", "GetCardInstance Card Reader count =  %ld", ReaderCount);
        unsigned long ReaderIdx = 0;
        long CardIdx = 0;
        unsigned long tempReaderIndex = 0;

        if (ReaderCount == 0) {
            emit signalCardAccessError(NoReaderFound);
        }
        else {
            PTEID_CardType lastFoundCardType = PTEID_CARDTYPE_UNKNOWN;

            // Count number of cards
            for (ReaderIdx = 0; ReaderIdx < ReaderCount; ReaderIdx++)
            {
                PTEID_ReaderContext& readerContext = ReaderSet.getReaderByNum(ReaderIdx);
                try
                {
                    if (readerContext.isCardPresent())
                    {
                        CardIdx++;
                        tempReaderIndex = ReaderIdx;
                    }
                }
                catch (PTEID_Exception &e){
                    unsigned long err = e.GetError();
                    PTEID_LOG(PTEID_LOG_LEVEL_DEBUG, "eidgui",
                        "Failed to check is Card Present 0x%08x\n", err);
                }
            }
            // Test if Card Reader was previously selected
            if (selectedReaderIndex != -1)
            {
                // Card Reader was previously selected
                PTEID_ReaderContext& readerContext = ReaderSet.getReaderByNum(selectedReaderIndex);
                PTEID_CardType CardType = readerContext.getCardType();
                lastFoundCardType = CardType;
                qDebug() << "Card Reader was previously selected CardType:" << CardType;
                PTEID_LOG(PTEID_LOG_LEVEL_DEBUG, "eidgui",
                    "Card Reader was previously selected CardType: %d", CardType);
                switch (CardType)
                {
                case PTEID_CARDTYPE_IAS07:
                {
                    PTEID_EIDCard& Card = readerContext.getEIDCard();
                    new_card = &Card;
                    break;
                }
                case PTEID_CARDTYPE_UNKNOWN:
                {
                    selectedReaderIndex = -1;
                    emit signalCardAccessError(CardUnknownCard);
                    break;
                }
                default:
                    break;
                }
            }
            else
            { //Card Reader was not previously selected
                if (CardIdx == 1)
                {
                    PTEID_ReaderContext& readerContext = ReaderSet.getReaderByNum(tempReaderIndex);
                    PTEID_CardType CardType = readerContext.getCardType();
                    lastFoundCardType = CardType;
                    qDebug() << "Card Reader was not previously selected CardType:" << CardType;
                    PTEID_LOG(PTEID_LOG_LEVEL_DEBUG, "eidgui",
                        "Card Reader was not previously selected CardType: %d", CardType);
                    switch (CardType)
                    {
                    case PTEID_CARDTYPE_IAS07:
                    {
                        PTEID_EIDCard& Card = readerContext.getEIDCard();
                        new_card = &Card;
                        break;
                    }

                    case PTEID_CARDTYPE_UNKNOWN:
                    {
                        selectedReaderIndex = -1;
                        emit signalCardAccessError(CardUnknownCard);
                        break;
                    }
                    default:
                        break;
                    }
                }
                else if (CardIdx > 1) //Card Reader was not previously selected
                {
                    qDebug() << "Card Reader was not previously selected. Ask user to select card";
                    PTEID_LOG(PTEID_LOG_LEVEL_DEBUG, "eidgui", "Card Reader was not previously selected. "
                        "Ask user to select card");
                    emit signalReaderContext(); // Ask user to select card
                }
                else
                {
                    emit signalCardAccessError(NoCardFound);
                }
            }
        }
    }
    catch (PTEID_ExParamRange &e)
    {
        PTEID_LOG(PTEID_LOG_LEVEL_DEBUG, "eidgui", "loadCardData failed with error code 0x%08x", e.GetError());
        emit signalCardAccessError(CardUnknownError);
    }
    catch (PTEID_ExNoCardPresent &e)
    {
        PTEID_LOG(PTEID_LOG_LEVEL_DEBUG, "eidgui", "loadCardData failed with error code 0x%08x", e.GetError());
        emit signalCardAccessError(NoCardFound);
    }
    catch (PTEID_ExCardChanged &e)
    {
        PTEID_LOG(PTEID_LOG_LEVEL_DEBUG, "eidgui", "loadCardData failed with error code 0x%08x", e.GetError());
        emit signalCardAccessError(CardUnknownError);
    }
    catch (PTEID_ExBadTransaction &e)
    {
        PTEID_LOG(PTEID_LOG_LEVEL_DEBUG, "eidgui", "loadCardData failed with error code 0x%08x", e.GetError());
        emit signalCardAccessError(CardUnknownError);
    }
    catch (PTEID_ExCertNoRoot &e)
    {
        PTEID_LOG(PTEID_LOG_LEVEL_DEBUG, "eidgui", "loadCardData failed with error code 0x%08x", e.GetError());
        emit signalCardAccessError(CardUnknownError);
    }
    catch (PTEID_Exception &e)
    {
        PTEID_LOG(PTEID_LOG_LEVEL_DEBUG, "eidgui", "loadCardData failed with error code 0x%08x", e.GetError());
        emit signalCardAccessError(CardUnknownError);
    }
}

void GAPI::setReaderByUser(unsigned long setReaderIndex){

    qDebug() << "AppController GAPI::setReader!" << setReaderIndex;

    if (setReaderIndex >= ReaderSet.readerCount()) {
        setReaderIndex = ReaderSet.readerCount() - 1;
    }
    selectedReaderIndex = setReaderIndex;
}

QVariantList GAPI::getRetReaderList()
{
    qDebug() << "AppController GAPI::getRetReaderList!";

    QVariantList list;

    try {
        const char* const* ReaderList = ReaderSet.readerList();

        for (unsigned long Idx = 0; Idx < ReaderSet.readerCount(); Idx++)
        {
            const char* readerName = ReaderList[Idx];
            list << readerName;
        }
        if (selectedReaderIndex >= 0) {
            if ((unsigned long)selectedReaderIndex >= ReaderSet.readerCount()) {
                selectedReaderIndex = ReaderSet.readerCount() - 1;
            }
            emit signalSetReaderComboIndex(selectedReaderIndex);
        }


    }catch (PTEID_Exception& e) {
        qDebug() << "Error getRetReaderList!";
    }

    return list;
}

int GAPI::getReaderIndex(void)
{
    qDebug() << "AppController GAPI::geReaderIndex!" << selectedReaderIndex;
    if (selectedReaderIndex >= 0) {

        return selectedReaderIndex;
    }
    return 0;
}

void GAPI::connectToCard() {

    PTEID_LOG(eIDMW::PTEID_LOG_LEVEL_DEBUG, "eidgui", "GetCardInstance connectToCard");

    BEGIN_TRY_CATCH

        PTEID_EIDCard * card = NULL;
    getCardInstance(card);
    if (card == NULL) return;

    card->doSODCheck(true); //Enable SOD checking
    PTEID_EId &eid_file = card->getID();

    qDebug() << "C++: loading Card Data";

    QMap<IDInfoKey, QString> cardData;

    cardData[Surname] = QString::fromUtf8(eid_file.getSurname());
    cardData[Givenname] = QString::fromUtf8(eid_file.getGivenName());
    cardData[Sex] = QString::fromUtf8(eid_file.getGender());
    cardData[Height] = QString::fromUtf8(eid_file.getHeight());
    cardData[Country] = QString::fromUtf8(eid_file.getCountry());
    cardData[Birthdate] = QString::fromUtf8(eid_file.getDateOfBirth());
    cardData[Father] = QString::fromUtf8(eid_file.getGivenNameFather()) + " " +
        QString::fromUtf8(eid_file.getSurnameFather());
    cardData[Mother] = QString::fromUtf8(eid_file.getGivenNameMother()) + " " +
        QString::fromUtf8(eid_file.getSurnameMother());
    cardData[AccidentalIndications] = QString::fromUtf8(eid_file.getAccidentalIndications());
    cardData[Documenttype] = QString::fromUtf8(eid_file.getDocumentType());
    cardData[Documentnum] = QString::fromUtf8(eid_file.getDocumentNumber());
    cardData[Documentversion] = QString::fromUtf8(eid_file.getDocumentVersion());
    cardData[Nationality] = QString::fromUtf8(eid_file.getNationality());
    cardData[Validityenddate] = QString::fromUtf8(eid_file.getValidityEndDate());
    cardData[Validitybegindate] = QString::fromUtf8(eid_file.getValidityBeginDate());
    cardData[PlaceOfRequest] = QString::fromUtf8(eid_file.getLocalofRequest());
    cardData[IssuingEntity] = QString::fromUtf8(eid_file.getIssuingEntity());
    cardData[NISS] = QString::fromUtf8(eid_file.getSocialSecurityNumber());
    cardData[NSNS] = QString::fromUtf8(eid_file.getHealthNumber());
    cardData[NIF] = QString::fromUtf8(eid_file.getTaxNo());
    cardData[NIC] = QString::fromUtf8(eid_file.getCivilianIdNumber());

    //Load photo into a QPixmap
    PTEID_ByteArray& photo = eid_file.getPhotoObj().getphoto();

    QPixmap image_photo;
    image_photo.loadFromData(photo.GetBytes(), photo.Size(), "PNG");

    image_provider->setPixmap(image_photo);

    //All data loaded: we can emit the signal to QML
    setDataCardIdentify(cardData);

    END_TRY_CATCH
}

//****************************************************
// Callback function used by the Readercontext to notify insertion/removal of a card
// The callback comes at:
// - startup
// - insertion of a card
// - removal of a card
// - add/remove of a cardreader
// When a card is inserted we post a signal to the GUI telling that
// a new card is inserted.
//****************************************************
void cardEventCallback(long lRet, unsigned long ulState, CallBackData* pCallBackData)
{
    g_runningCallback++;

    try
    {
        PTEID_ReaderContext& readerContext = ReaderSet.getReaderByName(pCallBackData->getReaderName().toLatin1());

        unsigned int cardState = (unsigned int)ulState & 0x0000FFFF;
        unsigned int eventCounter = ((unsigned int)ulState) >> 16;

        PTEID_LOG(PTEID_LOG_LEVEL_DEBUG, "eventCallback", "Card Event received for reader %s: cardState: %u Event Counter: %u",
            pCallBackData->getReaderName().toUtf8().constData(), cardState, eventCounter);

        //------------------------------------
        // is card removed from the reader?
        //------------------------------------
        if (!readerContext.isCardPresent())
        {

            if (pCallBackData->getMainWnd()->m_Settings.getRemoveCert())
            {
                PTEID_LOG(PTEID_LOG_LEVEL_DEBUG, "eventCallback", "Will try to RemoveCertificates...");
                bool bImported = pCallBackData->getMainWnd()->m_Certificates.RemoveCertificates(pCallBackData->getReaderName());

                if (!bImported) {
                    qDebug() << "RemoveCertificates fail";

                    PTEID_LOG(PTEID_LOG_LEVEL_DEBUG, "eventCallback", "RemoveCertificates failed!");
                    emit pCallBackData->getMainWnd()->signalRemoveCertificatesFail();
                }
            }

            //------------------------------------
            // send an event to the main app to show the popup message
            //------------------------------------
            if (pCallBackData->getMainWnd()->returnReaderSelected() != -1){
                pCallBackData->getMainWnd()->signalCardChanged(GAPI::ET_CARD_CHANGED);
            }else{
                pCallBackData->getMainWnd()->signalCardChanged(GAPI::ET_CARD_REMOVED);
            }
            pCallBackData->getMainWnd()->setAddressLoaded(false);
            pCallBackData->getMainWnd()->resetReaderSelected();

            g_runningCallback--;
            return;
        }
        //------------------------------------
        // is card inserted ?
        //------------------------------------
        else if (readerContext.isCardChanged(pCallBackData->m_cardID))
        {
            //------------------------------------
            // send an event to the main app to show the popup message
            //------------------------------------
            pCallBackData->getMainWnd()->signalCardChanged(GAPI::ET_CARD_CHANGED);
            pCallBackData->getMainWnd()->setAddressLoaded(false);
            pCallBackData->getMainWnd()->resetReaderSelected();

            const char * tmp = readerContext.getEIDCard().getVersionInfo().getSerialNumber();
            if (tmp) PTEID_LOG(PTEID_LOG_LEVEL_CRITICAL, "eidgui", 
                "Card inserted with serial number (last 3): %s", tmp + strlen(tmp) - 3);

            PTEID_LOG(PTEID_LOG_LEVEL_DEBUG, "eidgui",
                "Card inserted with serial number: %s", tmp);

            //------------------------------------
            // register certificates when needed
            //------------------------------------
            if (pCallBackData->getMainWnd()->m_Settings.getRegCert())
            {
                PTEID_CardType cardType = readerContext.getCardType();
                switch (cardType)
                {
                case PTEID_CARDTYPE_IAS07:
                case PTEID_CARDTYPE_IAS101:
                {
                    PTEID_LOG(PTEID_LOG_LEVEL_DEBUG, "eventCallback", "Will try to ImportCertificates...");
                    bool bImported = pCallBackData->getMainWnd()->m_Certificates.ImportCertificates(pCallBackData->getReaderName());

                    if (!bImported) {
                        PTEID_LOG(PTEID_LOG_LEVEL_ERROR, "eventCallback", "ImportCertificates failed!");
                        emit pCallBackData->getMainWnd()->signalImportCertificatesFail();
                    }
                    break;
                }
                case PTEID_CARDTYPE_UNKNOWN:
                    PTEID_LOG(PTEID_LOG_LEVEL_DEBUG, "eventCallback", "Unknown Card - skipping ImportCertificates()");
                    break;
                }
            }
        }
        else
        {
            PTEID_LOG(PTEID_LOG_LEVEL_DEBUG, "eventCallback", "Event ignored");
        }
    }
    catch (...)
    {
        emit pCallBackData->getMainWnd()->signalCardAccessError(GAPI::CardUnknownError);
        // we catch ALL exceptions. This is because otherwise the thread throwing the exception stops
    }
    g_runningCallback--;
}

//*****************************************************
// update the readerlist. In case a reader is added to the machine
// at runtime.
//*****************************************************
void GAPI::updateReaderList(void)
{
    //----------------------------------------------------
    // check if the number of readers is changed
    //----------------------------------------------------
    try
    {
        if (ReaderSet.isReadersChanged())
        {
            stopAllEventCallbacks();
            ReaderSet.releaseReaders();

            if (0 == ReaderSet.readerCount()){
                emit signalCardAccessError(NoReaderFound);
            }
        }
        if (0 == m_callBackHandles.size())
        {
            setEventCallbacks();
        }
    }
    catch (...)
    {
        stopAllEventCallbacks();
        ReaderSet.releaseReaders();
    }
}
void GAPI::setEventCallbacks(void)
{
    //----------------------------------------
    // for all the readers, create a callback such we can know
    // afterwards, which reader called us
    //----------------------------------------

    try
    {
        size_t maxcount = ReaderSet.readerCount(true);
        for (size_t Ix = 0; Ix<maxcount; Ix++)
        {
            void(*fCallback)(long lRet, unsigned long ulState, void* pCBData);

            const char*			 readerName = ReaderSet.getReaderName(Ix);
            PTEID_ReaderContext& readerContext = ReaderSet.getReaderByNum(Ix);
            CallBackData*		 pCBData = new CallBackData(readerName, this);

            fCallback = (void(*)(long, unsigned long, void *))&cardEventCallback;

            m_callBackHandles[readerName] = readerContext.SetEventCallback(fCallback, pCBData);
            m_callBackData[readerName] = pCBData;
        }
    }
    catch (PTEID_Exception& e)
    {
        emit signalCardChanged(ET_UNKNOWN);
    }
}

//*****************************************************
// stop the event callbacks and delete the corresponding callback data
// objects.
//*****************************************************
void GAPI::stopAllEventCallbacks(void)
{

    for (tCallBackHandles::iterator it = m_callBackHandles.begin()
        ; it != m_callBackHandles.end()
        ; it++
        )
    {
        PTEID_ReaderContext& readerContext = ReaderSet.getReaderByName(it.key().toLatin1());
        unsigned long handle = it.value();
        readerContext.StopEventCallback(handle);
    }
    m_callBackHandles.clear();
    cleanupCallbackData();
}

//*****************************************************
// cleanup the callback data
//*****************************************************
void GAPI::cleanupCallbackData(void)
{

    while (g_runningCallback)
    {
#ifdef WIN32
        ::Sleep(100);
#else
        ::usleep(100000);
#endif
    }

    g_cleaningCallback = true;

    for (tCallBackData::iterator it = m_callBackData.begin()
        ; it != m_callBackData.end()
        ; it++
        )
    {
        CallBackData* pCallbackData = it.value();
        delete pCallbackData;
    }
    m_callBackData.clear();
    g_cleaningCallback = false;
}

/* findCardCertificate returns the index i of the certificate to be obtained with certificates.getCert(i).
    This is because the copy constructor for PTEID_Certificate is not implemented.
*/
int GAPI::findCardCertificate(QString issuedBy, QString issuedTo) {
    PTEID_EIDCard * card = NULL;
    getCardInstance(card);
    if (card == NULL) {
        qDebug() << "Error: inspectCardCertificate should not be called if card is not inserted!";
        return -1;
    }
    PTEID_Certificates&	 certificates = card->getCertificates();
    size_t nCerts = certificates.countAll();
    for (size_t i = 0; i < nCerts; i++)
    {
        PTEID_Certificate &cert = certificates.getCert(i);
        std::string issuerName = cert.getIssuerName();
        std::string ownerName = cert.getOwnerName();
        if (issuerName == issuedBy.toUtf8().constData() && ownerName == issuedTo.toUtf8().constData())
        {
            return i;
        }
    }
    return -1;
}
void GAPI::viewCardCertificate(QString issuedBy, QString issuedTo) {
#ifdef WIN32
    PTEID_EIDCard * card = NULL;
    getCardInstance(card);
    if (card == NULL) {
        qDebug() << "Error: inspectCardCertificate should not be called if card is not inserted!";
        return;
    }
    PTEID_Certificates&	 certificates = card->getCertificates();
    int certIdx = findCardCertificate(issuedBy, issuedTo);
    if (certIdx < 0)
    {
        PTEID_LOG(eIDMW::PTEID_LOG_LEVEL_ERROR,
            "eidgui", "viewCardCertificate: could not find certificate");
        return;
    }
    PTEID_Certificate& cert = certificates.getCert(certIdx);
    PTEID_ByteArray certData = cert.getCertData();
    PCCERT_CONTEXT pCertContext = CertCreateCertificateContext(
        X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
        certData.GetBytes(),
        certData.Size());
    if (!pCertContext)
    {
        PTEID_LOG(eIDMW::PTEID_LOG_LEVEL_ERROR, 
            "eidgui", "inspectCardCertificate: Error creating cert context: %d", GetLastError());
        return;
    }

    HWND appWindow = GetForegroundWindow();
    CryptUIDlgViewContext(
        CERT_STORE_CERTIFICATE_CONTEXT,
        pCertContext,
        appWindow,
        NULL,
        0,
        NULL);
    if (pCertContext)
    {
        CertFreeCertificateContext(pCertContext);
    }
#endif
}
void GAPI::exportCardCertificate(QString issuedBy, QString issuedTo, QString outputPath) {
    Concurrent::run(this, &GAPI::doExportCardCertificate, issuedBy, issuedTo, outputPath);
}
void GAPI::doExportCardCertificate(QString issuedBy, QString issuedTo, QString outputPath) {
    PTEID_EIDCard * card = NULL;
    getCardInstance(card);
    if (card == NULL) {
        qDebug() << "Error: inspectCardCertificate should not be called if card is not inserted!";
        emit signalExportCertificates(false);
        return;
    }
    PTEID_Certificates&	 certificates = card->getCertificates();
    int certIdx = findCardCertificate(issuedBy, issuedTo);
    if (certIdx < 0)
    {
        PTEID_LOG(eIDMW::PTEID_LOG_LEVEL_ERROR,
            "eidgui", "doExportCardCertificate: could not find certificate");
        emit signalExportCertificates(false);
        return;
    }
    PTEID_Certificate& cert = certificates.getCert(certIdx);
    PTEID_ByteArray certData = cert.getCertData();

    QFile file(outputPath);
    file.open(QIODevice::WriteOnly);
    int bytesWritten = file.write((const char *)certData.GetBytes(), certData.Size());
    file.close();
    emit signalExportCertificates(bytesWritten == certData.Size());
}


void GAPI::buildTree(PTEID_Certificate &cert, bool &bEx, QVariantMap &certificatesMap)
{
    QVariantMap certificatesMapChildren;
    static int level = 0;
    static int status = PTEID_CERTIF_STATUS_UNKNOWN;

    if (cert.isRoot())
    {
        certificatesMapChildren.insert("OwnerName", cert.getOwnerName());
        certificatesMapChildren.insert("IssuerName", cert.getIssuerName());
        certificatesMapChildren.insert("ValidityBegin", cert.getValidityBegin());
        certificatesMapChildren.insert("ValidityEnd", cert.getValidityEnd());
        certificatesMapChildren.insert("KeyLength", QString::number(cert.getKeyLength()));
        if (status != PTEID_CERTIF_STATUS_CONNECT
            && status != PTEID_CERTIF_STATUS_ERROR){
            status = cert.getStatus();
        }
        certificatesMapChildren.insert("Status", status);

        certificatesMap.insert("level" + QString::number(level), certificatesMapChildren);
        certificatesMap.insert("levelCount", level + 1);
        level = 0;
        status = PTEID_CERTIF_STATUS_UNKNOWN;
    }
    else
    {
        certificatesMapChildren.insert("OwnerName", cert.getOwnerName());
        certificatesMapChildren.insert("IssuerName", cert.getIssuerName());
        certificatesMapChildren.insert("ValidityBegin", cert.getValidityBegin());
        certificatesMapChildren.insert("ValidityEnd", cert.getValidityEnd());
        certificatesMapChildren.insert("KeyLength", QString::number(cert.getKeyLength()));

        if (status != PTEID_CERTIF_STATUS_CONNECT
            && status != PTEID_CERTIF_STATUS_ERROR){
            status = cert.getStatus();
        }
        certificatesMapChildren.insert("Status", status);

        if (certificatesMap.contains("level" + QString::number(level))) {
            certificatesMap.insert("levelB" + QString::number(level), certificatesMapChildren);
        }
        else {
            certificatesMap.insert("level" + QString::number(level), certificatesMapChildren);
        }

        level++;

        try {
            buildTree(cert.getIssuer(), bEx, certificatesMap);
        } catch (PTEID_ExCertNoIssuer &ex) {
            bEx = true;
        }
    }
}

void GAPI::startfillCertificateList(void) {
    Concurrent::run(this, &GAPI::fillCertificateList);
}

void GAPI::startGetCardActivation(void) {

    Concurrent::run(this, &GAPI::getCertificateAuthStatus);
}

void GAPI::getCertificateAuthStatus(void)
{
    qDebug() << "getCertificateAuthStatus";

    QString returnString = getCardActivation();

    emit signalShowCardActivation(returnString);
}

void GAPI::fillCertificateList(void)
{
    bool noIssuer = false;
    QVariantMap certificatesMap;

	PTEID_LOG(eIDMW::PTEID_LOG_LEVEL_DEBUG, "eidgui", "GetCardInstance fillCertificateList");

    BEGIN_TRY_CATCH

        PTEID_EIDCard * card = NULL;
    getCardInstance(card);
    if (card == NULL) return;

    PTEID_Certificates&	 certificates = card->getCertificates();

    certificatesMap.clear();
    buildTree(certificates.getCert(PTEID_Certificate::CITIZEN_AUTH), noIssuer, certificatesMap);

    buildTree(certificates.getCert(PTEID_Certificate::CITIZEN_SIGN), noIssuer, certificatesMap);
    if (noIssuer)
    {
        PTEID_LOG(eIDMW::PTEID_LOG_LEVEL_DEBUG, "eidgui", "Certificate chain couldn't be completed!");
        qDebug() << "Certificate chain couldn't be completed!";
        emit signalCertificatesFail();
    } else {
        emit signalCertificatesChanged(certificatesMap);
    }

    END_TRY_CATCH
}

QString GAPI::getCachePath(void){
    return m_Settings.getPteidCachedir();
}

bool GAPI::customSignImageExist(void){
    QString path = m_Settings.getPteidCachedir() + "/CustomSignPicture.jpg";
    return fileExists(path);
}

//check if file exists and if yes: Is it really a file and no directory?

void GAPI::customSignRemove(void){
    QStringList paths = {m_Settings.getPteidCachedir() + "/CustomSignPicture.jpg",
                         m_Settings.getPteidCachedir() + "/CustomSignPicture_qml.jpg"};

    for (int i = 0; i < paths.size(); i++)
        QFile::remove(paths.at(i));
}

bool GAPI::useCustomSignature(void){
    QString customImagePath = m_Settings.getPteidCachedir() + "/CustomSignPicture.jpg";
    m_custom_image = QImage(customImagePath);

    if (m_Settings.getUseCustomSignature() && !m_custom_image.isNull())
    {
        qDebug() << "Using Custom Picture to sign a PDF";
        QBuffer buffer(&m_jpeg_scaled_data);
        buffer.open(QIODevice::WriteOnly);
        m_custom_image.save(&buffer, "JPG", 100);
        return true;
    }else{
        qDebug() << "Using default Picture to CC sign";
        return false;
    }
}
bool GAPI::saveCustomImageToCache(QString url){
    QString customImagePathCache = m_Settings.getPteidCachedir() + "/CustomSignPicture.jpg";

    if (QFile::exists(customImagePathCache))
    {
        QFile::remove(customImagePathCache);
    }

    QUrl source = QUrl(url);
    QImage custom_image = QImage(source.toLocalFile());

    // keep original resolution image to display in the application
    QString customImagePathCacheQml = m_Settings.getPteidCachedir() + "/CustomSignPicture_qml.jpg";
    if (QFile::exists(customImagePathCacheQml))
    {
        QFile::remove(customImagePathCacheQml);
    }
    // set white background so png images with transparency are displayed
    // in the application as they appear in the real signature seal
    QImage customImageWhiteBg(custom_image.width(), custom_image.height(), QImage::Format_RGB32);
    customImageWhiteBg.fill(QColor(Qt::white).rgb());
    QPainter painterAux(&customImageWhiteBg);
    painterAux.drawImage(0, 0, custom_image);
    customImageWhiteBg.save(customImagePathCacheQml, "JPG", 100);

    int MAX_IMAGE_HEIGHT = 41;
    int MAX_IMAGE_WIDTH = 185;
    float RECOMMENDED_RATIO = MAX_IMAGE_HEIGHT / (float) MAX_IMAGE_WIDTH;
    float ACTUAL_RATIO = custom_image.height() / (float) custom_image.width();

    // Only scale image when needed
    if(custom_image.height() != MAX_IMAGE_HEIGHT || custom_image.width() != MAX_IMAGE_WIDTH)
    {
        // Fit custom image to available space, keeping aspect ratio
        if (ACTUAL_RATIO >= RECOMMENDED_RATIO)
            custom_image = custom_image.scaledToHeight(MAX_IMAGE_HEIGHT, Qt::SmoothTransformation);
        else
            custom_image = custom_image.scaledToWidth(MAX_IMAGE_WIDTH, Qt::SmoothTransformation);
    }

    // Create blank image
    QImage final_img = QImage(MAX_IMAGE_WIDTH, MAX_IMAGE_HEIGHT, QImage::Format_RGB32);
    final_img.fill(QColor(Qt::white).rgb());
    QPainter painter(&final_img);

    // Center scaled custom image in the white background
    int position_x = (int) ((MAX_IMAGE_WIDTH/2) - custom_image.width()/2);
    int position_y = (int) ((MAX_IMAGE_HEIGHT/2) - custom_image.height()/2);
    painter.drawImage(position_x, position_y, custom_image);

    // Save custom image in cache, ready to be applied in a signature
    return final_img.save(customImagePathCache, "JPG", 100);
}
void GAPI::setUseCustomSignature(bool UseCustomSignature){

    m_Settings.setUseCustomSignature(UseCustomSignature);
}
bool GAPI::getUseCustomSignature(void){
    return m_Settings.getUseCustomSignature();
}
void GAPI::setRegCertValue(bool bRegCert){

    m_Settings.setRegCert(bRegCert);
}
void GAPI::setRemoveCertValue(bool bRemoveCert){

    m_Settings.setRemoveCert(bRemoveCert);
}
bool GAPI::getRegCertValue(void){

    return m_Settings.getRegCert();
}
bool GAPI::getRemoveCertValue(void){

    return m_Settings.getRemoveCert();
}

#ifdef WIN32
QVariantList GAPI::getRegisteredCmdPhoneNumbers() {
    QVariantList regCmdNumList;

    std::vector<std::string> phoneNums;
    m_cmdCertificates->GetRegisteredPhoneNumbers(&phoneNums);

    for (size_t i = 0; i < phoneNums.size(); i++)
    {
        regCmdNumList << phoneNums[i].c_str();
    }
    return regCmdNumList;
}
#endif

void GAPI::doCancelCMDRegisterCert() {
#ifdef WIN32
    m_cmdCertificates->CancelImport();
#endif
}
void GAPI::cancelCMDRegisterCert() {
#ifdef WIN32
    Concurrent::run(this, &GAPI::doCancelCMDRegisterCert);
#endif
}

void GAPI::registerCMDCertOpen(QString mobileNumber, QString pin) {
#ifdef WIN32
    Concurrent::run(this, &GAPI::doRegisterCMDCertOpen, mobileNumber, pin);
#endif
}
void GAPI::doRegisterCMDCertOpen(QString mobileNumber, QString pin) {
#ifdef WIN32
    qDebug() << "Register CMD Cert of number " << mobileNumber;
    signalUpdateProgressBar(25);
    signalUpdateProgressStatus(tr("STR_CMD_CONNECTING"));
    int res = m_cmdCertificates->ImportCertificatesOpen(mobileNumber.toStdString(), pin.toStdString());

    QString error_msg;
    QString urlLink = "";
    // errors in cmdErrors.h
    switch (res) {
    case ERR_NONE:
        error_msg = tr("STR_CMD_LOGIN_SUCESS");
        signalUpdateProgressBar(50);
        signalUpdateProgressStatus(error_msg);
        emit signalValidateOtp();
        return;
    case SOAP_TCP_ERROR:
        error_msg = tr("STR_VERIFY_INTERNET");
        if (m_Settings.isProxyConfigured())
            error_msg.append(" ").append(tr("STR_VERIFY_PROXY"));
        break;
    case ERR_GET_CERTIFICATE:
        urlLink = tr("STR_URL_AUTENTICACAO_GOT_PT");
        error_msg = tr("STR_CMD_GET_CERTIFICATE_ERROR");
        break;
    default:
        urlLink = tr("STR_URL_AUTENTICACAO_GOT_PT");
        error_msg = tr("STR_CERT_REG_ERROR");
    }
    signalUpdateProgressBar(100);
    signalUpdateProgressStatus(tr("STR_POPUP_ERROR") + "!");
    emit signalShowMessage(error_msg, urlLink);
#endif
}
void GAPI::registerCMDCertClose(QString otp) {
#ifdef WIN32
    Concurrent::run(this, &GAPI::doRegisterCMDCertClose, otp);
#endif
}
void GAPI::doRegisterCMDCertClose(QString otp) {
#ifdef WIN32
    signalUpdateProgressBar(75);
    signalUpdateProgressStatus(tr("STR_CMD_SENDING_CODE"));
    int res = m_cmdCertificates->ImportCertificatesClose(otp.toStdString());

    QString top_msg = tr("STR_POPUP_ERROR") + "!";
    QString error_msg;
    QString urlLink = "";
    // errors in cmdErrors.h
    switch (res) {
    case ERR_NONE:
        top_msg = tr("STR_POPUP_SUCESS") + "!";
        error_msg = tr("STR_CERT_REG_SUCC");
        m_Settings.setAskToRegisterCmdCert(false);
        break;
    case SOAP_TCP_ERROR:
        error_msg = tr("STR_VERIFY_INTERNET");
        if (m_Settings.isProxyConfigured())
            error_msg.append(" ").append(tr("STR_VERIFY_PROXY"));
        break;
    default:
        error_msg = tr("STR_CERT_REG_ERROR");
        urlLink = tr("STR_URL_AUTENTICACAO_GOT_PT");
    }

    signalUpdateProgressBar(100);
    signalUpdateProgressStatus(top_msg);
    emit signalShowMessage(error_msg, urlLink);
#endif
}

bool GAPI::areRootCACertsInstalled() {
#ifdef WIN32
    return Concurrent::run(&CERTIFICATES::IsNewRootCACertInstalled);
#else
    return false;
#endif
}

void GAPI::installRootCACert() {
#ifdef WIN32
    Concurrent::run(this, &GAPI::doInstallRootCACert);
#endif
}


void GAPI::doInstallRootCACert() {
#ifdef WIN32
    bool installed = m_Certificates.InstallNewRootCa();
    emit signalInstalledRootCACert(installed);
#endif
}

void GAPI::quitApplication(bool restart) {
    try
    {
        if (m_Settings.getRemoveCert())
        {
            for (unsigned long readerCount = 0; readerCount<ReaderSet.readerCount(); readerCount++)
            {
                QString readerName = ReaderSet.getReaderName(readerCount);
                m_Certificates.RemoveCertificates(readerName);
            }
        }

        //-------------------------------------------------------------------
        // we must release all the certificate contexts before releasing the SDK.
        // After Release, no more calls should be done to the SDK and as such
        // noting should be done in the dtor
        //-------------------------------------------------------------------
        forgetAllCertificates();
        stopAllEventCallbacks();

    }
    catch (...) {}
    if (!Concurrent::waitForAll(30)) {
        // Threads did not finished and timeout occured: exit process
        // Some threads may not have been correctly finished.
        PTEID_LOG(eIDMW::PTEID_LOG_LEVEL_CRITICAL, "eidgui",
            "Exiting application after timeout.");
    }
    qApp->exit((restart ? RESTART_EXIT_CODE : SUCCESS_EXIT_CODE));

}

//*****************************************************
// forget all the certificates we kept for all readers
//*****************************************************
void GAPI::forgetAllCertificates(void)
{
#ifdef WIN32
    bool bRefresh = true;;
    for (unsigned long readerIdx = 0; readerIdx<ReaderSet.readerCount(bRefresh); readerIdx++)
    {
        const char* readerName = ReaderSet.getReaderByNum(readerIdx).getName();
        forgetCertificates(readerName);
    }
#endif
}

//*****************************************************
// forget all the certificates we kept for a specific reader
//*****************************************************
void GAPI::forgetCertificates(QString const& reader)
{
    char readerName[256];
    readerName[0] = 0;
    if (reader.length()>0)
    {
        strcpy(readerName, reader.toUtf8().data());
    }
#ifdef WIN32
    QVector<PCCERT_CONTEXT> readerCertContext = m_Certificates.m_certContexts[readerName];
    while (0 < readerCertContext.size())
    {
        PCCERT_CONTEXT pContext = readerCertContext[readerCertContext.size() - 1];
        CertFreeCertificateContext(pContext);
        readerCertContext.erase(readerCertContext.end() - 1);
    }
#endif
}

void GAPI::setAppAsDlgParent() {
#ifdef WIN32
    HWND appWindow = GetForegroundWindow();
    SetApplicationWindow(appWindow);
#endif
}

bool GAPI::checkCMDSupport() {
#if CMD_SUPPORT
    return true;
#else
    return false;
#endif
}

QString GAPI::getAbsolutePath(QString path) {
    QFileInfo fileInfo(path);
    if (fileInfo.exists() && fileInfo.isRelative())
    {
        return fileInfo.absoluteFilePath();
    }
    return path;
}