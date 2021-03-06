/**
 ******************************************************************************
 * @file       kmlexport.cpp
 * @author     Tau Labs, http://taulabs.org, Copyright (C) 2013.
 * @brief Exports log data to KML
 * @addtogroup GCSPlugins GCS Plugins
 * @{
 * @addtogroup KmlExportPlugin
 * @{
 *****************************************************************************/
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include <QDebug>
#include <QMessageBox>
#include <QTextStream>
#include <QtGlobal>

#include <coreplugin/coreconstants.h>
#include "utils/coordinateconversions.h"
#include "uavobjects/uavobjectsinit.h"
#include "uavobjectmanager.h"

#include "kmlexport.h"

QString KmlExport::dateTimeFormat="yyyy-MM-ddThh:mm:ssZ"; // XML Schema time format. Required by KML specification
const double ColorMap_Jet[256][3] = COLORMAP_JET;

#define maxVelocity 20 // Vehicle velocity which corresponds to maximum color in color map. This shouldn't be hardcoded
#define numberOfWallAxes 5 // Number of wall axes to plot. This shouldn't be hardcoded
#define wallAxesSeparation 20 // Wall axes separation height in [m]. This shouldn't be hardcoded


KmlExport::KmlExport(QString inputLogFileName, QString outputKmlFileName) :
    outputFileName(outputKmlFileName)
{
    logFile.setFileName(inputLogFileName);

    // Create new UAVObject manager and initialize it with all UAVObjects
    UAVObjectManager *kmlUAVObjectManager = new UAVObjectManager;
    UAVObjectsInitialize(kmlUAVObjectManager);

    // Connect new UAVO manager to a UAVTalk instance
    kmlTalk = new UAVTalk(&logFile, kmlUAVObjectManager);

    // Get the UAVObjects
    airspeedActual = AirspeedActual::GetInstance(kmlUAVObjectManager);
    attitudeActual = AttitudeActual::GetInstance(kmlUAVObjectManager);
    gpsPosition = GPSPosition::GetInstance(kmlUAVObjectManager);
    homeLocation = HomeLocation::GetInstance(kmlUAVObjectManager);
    positionActual = PositionActual::GetInstance(kmlUAVObjectManager);
    velocityActual = VelocityActual::GetInstance(kmlUAVObjectManager);

    homeLocationData = homeLocation->getData();
    gpsPositionData = gpsPosition->getData();

    // Connect position actual. This is the trigger event for plotting a new
    // KML placemark.
    connect(positionActual, SIGNAL(objectUpdated(UAVObject *)), this, SLOT(positionActualUpdated(UAVObject *)), Qt::DirectConnection);
    connect(homeLocation, SIGNAL(objectUpdated(UAVObject *)), this, SLOT(homeLocationUpdated(UAVObject *)), Qt::DirectConnection);
    connect(gpsPosition, SIGNAL(objectUpdated(UAVObject *)), this, SLOT(gpsPositionUpdated(UAVObject *)), Qt::DirectConnection);

    // Get the factory singleton to create KML elements.
    factory = KmlFactory::GetFactory();

    // Create <Document>
    document = factory->CreateDocument();

    // Create folders
    timestampFolder = factory->CreateFolder();
    timestampFolder->set_name("Arrows");

    trackFolder = factory->CreateFolder();
    trackFolder->set_name("Track");

    // Create custom styles. Add as document's first elements
    StyleMapPtr styleCBS = createCustomBalloonStyle();
    document->add_styleselector(styleCBS);

    StylePtr styleGT = createGroundTrackStyle();
    document->add_styleselector(styleGT);

    StyleMapPtr wallAxesStyle = createWallAxesStyle();
    document->add_styleselector(wallAxesStyle);

    // Create an array of lines which will make the wall axes.
    for (int i=0; i<numberOfWallAxes; i++){
        CoordinatesPtr coordinates = factory->CreateCoordinates();
        wallAxes.append(coordinates);
    }
}


/**
 * @brief KmlExport::exportToKML Triggers logfile export to KML.
 */
bool KmlExport::exportToKML()
{
    bool ret = open();
    if (!ret) {
        qDebug () << "Logfile failed to open during KML export";
        return false;
    }

    // Parses logfile and generates KML document
    ret = preparseLogFile();
    if (!ret) {
        qDebug () << "Logfile preparsing failed";
        return false;
    }

    // Call parser.
    parseLogFile();

    // Add track to <Document>
    document->add_feature(trackFolder);

    // Add timespans to <Document>
    document->add_feature(timestampFolder);

    // Add ground track to <Document>
    {
        LineStringPtr linestring = factory->CreateLineString();
        linestring->set_extrude(false); // Do not extrude to ground
        linestring->set_altitudemode(kmldom::ALTITUDEMODE_CLAMPTOGROUND);
        linestring->set_coordinates(wallAxes[0]);

        MultiGeometryPtr multiGeometry = factory->CreateMultiGeometry();
        multiGeometry->add_geometry(linestring);

        PlacemarkPtr placemark = factory->CreatePlacemark();
        placemark->set_geometry(multiGeometry);
        placemark->set_styleurl("#ts_2_tb");
        placemark->set_name("Ground track");

        document->add_feature(placemark);
    }

    // Add wall axes to <Document>
    FolderPtr folder = factory->CreateFolder();
    for (int i=0; i<numberOfWallAxes; i++) {
        LineStringPtr linestring = factory->CreateLineString();
        linestring->set_extrude(false); // Do not extrude to ground
        linestring->set_altitudemode(kmldom::ALTITUDEMODE_ABSOLUTE);
        linestring->set_coordinates(wallAxes[i]);

        MultiGeometryPtr multiGeometry = factory->CreateMultiGeometry();
        multiGeometry->add_geometry(linestring);

        PlacemarkPtr placemark = factory->CreatePlacemark();
        placemark->set_geometry(multiGeometry);
        placemark->set_styleurl("#ts_1_tb");

        folder->add_feature(placemark);
        folder->set_name("Wall axes");
    }
    document->add_feature(folder);

    // Create <kml> and give it <Document>.
    KmlPtr kml = factory->CreateKml();
    kml->set_feature(document);  // kml takes ownership.

    // Serialize to XML
    std::string kml_data = kmldom::SerializePretty(kml);

    // Save to file
    if (QFileInfo(outputFileName).suffix().toLower() == "kmz") {
        if (!kmlengine::KmzFile::WriteKmz(outputFileName.toStdString().c_str(), kml_data)) {
            qDebug() << "KMZ write failed: " << outputFileName;
            QMessageBox::critical(new QWidget(),"KMZ write failed", "Failed to write KMZ file.");
            return false;
        }
    } else if (QFileInfo(outputFileName).suffix().toLower() == "kml") {
        if (!kmlbase::File::WriteStringToFile(kml_data, outputFileName.toStdString())) {
            qDebug() << "KML write failed: " << outputFileName;
            QMessageBox::critical(new QWidget(),"KML write failed", "Failed to write KML file.");
            return false;
        }
    } else {
        qDebug() << "Write failed. Invalid file name:" << outputFileName;
        QMessageBox::critical(new QWidget(),"Write failed", "Failed to write file. Invalid filename");
        return false;
    }


    return true;
}


/**
 * @brief KmlExport::open Opens the logfile and ensures it's sane
 * @return returns true if the logfile is successfully opened, returns false otherwise.
 */
bool KmlExport::open()
{
    if (logFile.isOpen()) {
        logFile.close();
    }

    //Open log file as  ReadOnly
    if(logFile.open(QIODevice::ReadOnly) == false)
    {
        qDebug() << "Unable to open " << logFile.fileName() << " for logging";
        return false;
    }

    logFile.readLine(); //Read first line of log file. This assumes that the logfile is of the new format.
    QString logGitHashString=logFile.readLine().trimmed(); //Read second line of log file. This assumes that the logfile is of the new format.
    QString logUAVOHashString=logFile.readLine().trimmed(); //Read third line of log file. This assumes that the logfile is of the new format.
    QString gitHash = QString::fromLatin1(Core::Constants::GCS_REVISION_STR);
    QString uavoHash = QString::fromLatin1(Core::Constants::UAVOSHA1_STR).replace("\"{ ", "").replace(" }\"", "").replace(",", "").replace("0x", ""); // See comment above for necessity for string replacements

    if(logUAVOHashString != uavoHash){
        QMessageBox msgBox;
        msgBox.setText("Likely log file incompatibility.");
        msgBox.setInformativeText(QString("The log file was made with branch %1, UAVO hash %2. GCS will attempt to export the file.").arg(logGitHashString).arg(logUAVOHashString));
        msgBox.exec();
    }
    else if(logGitHashString != gitHash){
        QMessageBox msgBox;
        msgBox.setText("Possible log file incompatibility.");
        msgBox.setInformativeText(QString("The log file was made with branch %1. GCS will attempt to export the file.").arg(logGitHashString));
        msgBox.exec();
    }

    QString tmpLine=logFile.readLine(); //Look for the header/body separation string.
    int cnt=0;
    while (tmpLine!="##\n" && cnt < 10 && !logFile.atEnd()){
        tmpLine=logFile.readLine().trimmed();
        cnt++;
    }

    //Check if we reached the end of the file before finding the separation string
    if (cnt >=10 || logFile.atEnd()){
        QMessageBox msgBox;
        msgBox.setText("Corrupted file.");
        msgBox.setInformativeText("GCS cannot find the separation byte. GCS will attempt to export the file."); //<--TODO: add hyperlink to webpage with better description.
        msgBox.exec();

        //Since we could not find the file separator, we need to return to the beginning of the file
        logFile.seek(0);
    }

    return true;
}


/**
 * @brief KmlExport::preparseLogFile Ensures that the logfile has data to export
 * @return Returns true if the logfile has data, false otherwise
 */
bool KmlExport::preparseLogFile()
{
    //Read all log timestamps into array
    timestampBuffer.clear(); //Save beginning of log for later use
    timestampPos.clear();
    quint64 logFileStartIdx = logFile.pos();
    quint32 lastTimeStamp = 0;

    while (!logFile.atEnd()){
        qint64 dataSize;

        //Get time stamp position
        timestampPos.append(logFile.pos());

        //Read timestamp and logfile packet size
        logFile.read((char *) &lastTimeStamp, sizeof(lastTimeStamp));
        logFile.read((char *) &dataSize, sizeof(dataSize));

        //Check if dataSize sync bytes are correct.
        //TODO: LIKELY AS NOT, THIS WILL FAIL TO RESYNC BECAUSE THERE IS TOO LITTLE INFORMATION IN THE STRING OF SIX 0x00
        if ((dataSize & 0xFFFFFFFFFFFF0000)!=0){
            qDebug() << "Wrong sync byte. At file location 0x"  << QString("%1").arg(logFile.pos(),0,16) << "Got 0x" << QString("%1").arg(dataSize & 0xFFFFFFFFFFFF0000,0,16) << ", but expected 0x""00"".";
            logFile.seek(timestampPos.last()+1);
            timestampPos.pop_back();
            continue;
        }

        //Check if timestamps are sequential.
        if (!timestampBuffer.isEmpty() && lastTimeStamp < timestampBuffer.last()){
            QMessageBox msgBox;
            msgBox.setText("Corrupted file.");
            msgBox.setInformativeText("Timestamps are not sequential. Playback may have unexpected behavior"); //<--TODO: add hyperlink to webpage with better description.
            msgBox.exec();

            qDebug() << "Timestamp: " << timestampBuffer.last() << " " << lastTimeStamp;
        }

        timestampBuffer.append(lastTimeStamp);

        logFile.seek(timestampPos.last()+sizeof(lastTimeStamp)+sizeof(dataSize)+dataSize);
    }

    //Check if any timestamps were successfully read
    if (timestampBuffer.size() == 0){
        QMessageBox msgBox;
        msgBox.setText("Empty logfile.");
        msgBox.setInformativeText("No log data can be found.");
        msgBox.exec();

        stopExport();
        return false;
    }

    //Reset to log beginning, including the timestamp.
    logFile.seek(logFileStartIdx);

    return true;
}


/**
 * @brief KmlExport::stopExport Called to stop the export. Currently only closes
 * the logfile
 * @return Returns true
 */
bool KmlExport::stopExport()
{
    logFile.close();
    return true;
}


/**
 * @brief KmlExport::parseLogFile Parses logfile and exports results to KML file
 */
void KmlExport::parseLogFile()
{
    qint64 packetSize;
    quint32 timeStampIdx;

    //Read packets
    while (!logFile.atEnd())
    {
        if(logFile.bytesAvailable() < 4) {
            break;
        }

        //Read timestamp and logfile packet size
        logFile.read((char *) &timeStamp, sizeof(timeStamp));
        logFile.read((char *) &packetSize, sizeof(packetSize));

        if (packetSize<1 || packetSize>(1024*1024)) {
            qDebug() << "Error: Logfile corrupted! Unlikely packet size: " << packetSize << "\n";
            QMessageBox::critical(new QWidget(),"Corrupted file", "Incorrect packet size. Stopping export. Data up to this point will be saved.");

            break;
        }

        if(logFile.bytesAvailable() < packetSize) {
            break;
        }

        // Read the data packet from the file.
        QByteArray dataBuffer;
        dataBuffer.append(logFile.read(packetSize));

        // Parse the packet. This operation passes the data to the kmlTalk object, which internally parses the data
        // and then emits objectUpdated(UAVObject *) signals. These signals are connected to in the KmlExport constructor.
        for (int i=0; i < dataBuffer.size(); i++) {
            kmlTalk->processInputByte(dataBuffer[i]);
        }

        timeStampIdx++;
    }

    stopExport();
}


/**
 * @brief KmlExport::createCustomBalloonStyle Creates a custom balloon stye, using an arrow as an icon.
 * @return Returns the custom balloon style.
 */
StyleMapPtr KmlExport::createCustomBalloonStyle()
{
    StyleMapPtr styleMap = factory->CreateStyleMap();

    {

        // Add custom balloon style (gets rid of "Directions to here...")
        // https://groups.google.com/forum/?fromgroups#!topic/kml-support-getting-started/2CqF9oiynRY
        BalloonStylePtr balloonStyle = factory->CreateBalloonStyle();
        balloonStyle->set_text("$[description]");

        // Change the icon
        IconStyleIconPtr iconStyleIcon = factory->CreateIconStyleIcon();
        iconStyleIcon->set_href("http://maps.google.com/mapfiles/kml/shapes/arrow.png");

        // Create a label style
        LabelStylePtr labelStyle = factory->CreateLabelStyle();
        labelStyle->set_color(kmlbase::Color32(255, 0, 255, 255));
        labelStyle->set_scale(0.75);

        // Create an icon style
        IconStylePtr iconStyle = factory->CreateIconStyle();
        iconStyle->set_icon(iconStyleIcon);
        iconStyle->set_scale(0.65);

        // Create a line style
        LineStylePtr lineStyle = factory->CreateLineStyle();
        lineStyle->set_width(3.25);

        // Link the style to the icon
        StylePtr style = factory->CreateStyle();
        style->set_balloonstyle(balloonStyle);
        style->set_iconstyle(iconStyle);
        style->set_linestyle(lineStyle);
        style->set_labelstyle(labelStyle);

        PairPtr pair = factory->CreatePair();
        pair->set_styleselector(style);
        pair->set_key(kmldom::STYLESTATE_NORMAL);

        styleMap->add_pair(pair);
    }

    {
        // Add custom balloon style (gets rid of "Directions to here...")
        // https://groups.google.com/forum/?fromgroups#!topic/kml-support-getting-started/2CqF9oiynRY
        BalloonStylePtr balloonStyle = factory->CreateBalloonStyle();
        balloonStyle->set_text("$[description]");

        // Change the icon
        IconStyleIconPtr iconStyleIcon = factory->CreateIconStyleIcon();
        iconStyleIcon->set_href("http://maps.google.com/mapfiles/kml/shapes/arrow.png");

        // Create an icon style
        IconStylePtr iconStyle = factory->CreateIconStyle();
        iconStyle->set_icon(iconStyleIcon);
        iconStyle->set_scale(0.65);

        // Create a label style
        LabelStylePtr labelStyle = factory->CreateLabelStyle();
        labelStyle->set_color(kmlbase::Color32(255, 0, 255, 255));
        labelStyle->set_scale(0.9);

        // Create a line style
        LineStylePtr lineStyle = factory->CreateLineStyle();
        lineStyle->set_width(6.5);

        // Link the style to the icon
        StylePtr style = factory->CreateStyle();
        style->set_balloonstyle(balloonStyle);
        style->set_iconstyle(iconStyle);
        style->set_linestyle(lineStyle);
        style->set_labelstyle(labelStyle);

        PairPtr pair = factory->CreatePair();
        pair->set_styleselector(style);
        pair->set_key(kmldom::STYLESTATE_HIGHLIGHT);

        styleMap->add_pair(pair);
    }

    styleMap->set_id("directiveArrowStyle");

    return styleMap;
}


/**
 * @brief KmlExport::createGroundTrackStyle Creates a custom style for the ground track.
 * @return Returns the custom style.
 */
StylePtr KmlExport::createGroundTrackStyle()
{
    // Add custom balloon style (gets rid of "Directions to here...")
    // https://groups.google.com/forum/?fromgroups#!topic/kml-support-getting-started/2CqF9oiynRY
    BalloonStylePtr balloonStyle = factory->CreateBalloonStyle();
    balloonStyle->set_text("$[id]");

    // Create an icon style
    IconStylePtr iconStyle = factory->CreateIconStyle();
    iconStyle->set_scale(0);

    // Create a label style
    LabelStylePtr labelStyle = factory->CreateLabelStyle();
    labelStyle->set_color(kmlbase::Color32(255, 0, 255, 255));
    labelStyle->set_scale(0);

    // Create a line style
    LineStylePtr lineStyle = factory->CreateLineStyle();
    lineStyle->set_color(kmlbase::Color32(255, 0, 0, 0)); // Black
    lineStyle->set_width(9);

    // Link the style to the icon
    StylePtr style = factory->CreateStyle();
    style->set_balloonstyle(balloonStyle);
    style->set_iconstyle(iconStyle);
    style->set_linestyle(lineStyle);
    style->set_labelstyle(labelStyle);

    style->set_id("ts_2_tb");

    return style;
}

StyleMapPtr KmlExport::createWallAxesStyle()
{
    StyleMapPtr styleMap = factory->CreateStyleMap();

    {
        // Add custom balloon style (gets rid of "Directions to here...")
        // https://groups.google.com/forum/?fromgroups#!topic/kml-support-getting-started/2CqF9oiynRY
        BalloonStylePtr balloonStyle = factory->CreateBalloonStyle();
        balloonStyle->set_text("$[id]");

        // Create an icon style
        IconStylePtr iconStyle = factory->CreateIconStyle();
        iconStyle->set_scale(0);

        // Create a label style
        LabelStylePtr labelStyle = factory->CreateLabelStyle();
        labelStyle->set_color(kmlbase::Color32(255, 0, 255, 255));
        labelStyle->set_scale(0);

        // Create a line style
        LineStylePtr lineStyle = factory->CreateLineStyle();
        lineStyle->set_color(kmlbase::Color32(255, 0, 0, 0)); // Black
        lineStyle->set_width(.9);

        // Link the style to the icon
        StylePtr style = factory->CreateStyle();
        style->set_balloonstyle(balloonStyle);
        style->set_iconstyle(iconStyle);
        style->set_linestyle(lineStyle);
        style->set_labelstyle(labelStyle);

        PairPtr pair = factory->CreatePair();
        pair->set_styleselector(style);
        pair->set_key(kmldom::STYLESTATE_NORMAL);

        styleMap->add_pair(pair);
    }

    {
        // Add custom balloon style (gets rid of "Directions to here...")
        // https://groups.google.com/forum/?fromgroups#!topic/kml-support-getting-started/2CqF9oiynRY
        BalloonStylePtr balloonStyle = factory->CreateBalloonStyle();
        balloonStyle->set_text("$[id]");

        // Create an icon style
        IconStylePtr iconStyle = factory->CreateIconStyle();
        iconStyle->set_scale(0);

        // Create a label style
        LabelStylePtr labelStyle = factory->CreateLabelStyle();
        labelStyle->set_color(kmlbase::Color32(255, 0, 255, 255));
        labelStyle->set_scale(0.75);

        // Create a line style
        LineStylePtr lineStyle = factory->CreateLineStyle();
        lineStyle->set_color(kmlbase::Color32(255, 0, 0, 0)); // Black
        lineStyle->set_width(1.8);

        // Link the style to the icon
        StylePtr style = factory->CreateStyle();
        style->set_balloonstyle(balloonStyle);
        style->set_iconstyle(iconStyle);
        style->set_linestyle(lineStyle);
        style->set_labelstyle(labelStyle);

        PairPtr pair = factory->CreatePair();
        pair->set_styleselector(style);
        pair->set_key(kmldom::STYLESTATE_HIGHLIGHT);

        styleMap->add_pair(pair);
    }

    styleMap->set_id("ts_1_tb");


    return styleMap;
}


/**
 * @brief KmlExport::CreateLineStringPlacemark Adds a line segment which is colored according to the
 * vehicle's speed.
 * @param startPoint Beginning point along line
 * @param endPoint End point point along line
 * @return Returns the placemark containing the line segment
 */
PlacemarkPtr KmlExport::CreateLineStringPlacemark(const LLAVCoordinates &startPoint, const LLAVCoordinates &endPoint, quint32 newPlacemarkTime)
{
    CoordinatesPtr coordinates = factory->CreateCoordinates();
    coordinates->add_latlngalt(startPoint.latitude, startPoint.longitude, startPoint.altitude);
    coordinates->add_latlngalt(endPoint.latitude,   endPoint.longitude,   endPoint.altitude);

    LineStringPtr linestring = factory->CreateLineString();
    linestring->set_extrude(true); // Extrude to ground
    linestring->set_altitudemode(kmldom::ALTITUDEMODE_ABSOLUTE);
    linestring->set_coordinates(coordinates);

    StyleMapPtr styleMap = factory->CreateStyleMap();


    // Add custom balloon style (gets rid of "Directions to here...")
    // https://groups.google.com/forum/?fromgroups#!topic/kml-support-getting-started/2CqF9oiynRY
    BalloonStylePtr balloonStyle = factory->CreateBalloonStyle();
    balloonStyle->set_text("$[description]");

    {
        double currentVelocity = (startPoint.groundspeed + endPoint.groundspeed)/2;

        // Set the linestyle. The color is a function of speed.
        LineStylePtr lineStyle = factory->CreateLineStyle();
        lineStyle->set_color(mapVelocity2Color(currentVelocity));

        PolyStylePtr polyStyle = factory->CreatePolyStyle();
        polyStyle->set_color(mapVelocity2Color(currentVelocity, 100));

        // Link the style to the icon
        StylePtr style = factory->CreateStyle();
        style->set_balloonstyle(balloonStyle);
        style->set_linestyle(lineStyle);
        style->set_polystyle(polyStyle);

        PairPtr pair = factory->CreatePair();
        pair->set_styleselector(style);
        pair->set_key(kmldom::STYLESTATE_NORMAL);

        styleMap->add_pair(pair);
    }

    {
        double currentVelocity = (startPoint.groundspeed + endPoint.groundspeed)/2;

        // Set the linestyle. The color is a function of speed.
        LineStylePtr lineStyle = factory->CreateLineStyle();
        lineStyle->set_color(mapVelocity2Color(currentVelocity));

        PolyStylePtr polyStyle = factory->CreatePolyStyle();
        polyStyle->set_color(mapVelocity2Color(currentVelocity, 100));
        polyStyle->set_fill(false);

        // Link the style to the icon
        StylePtr style = factory->CreateStyle();
        style->set_balloonstyle(balloonStyle);
        style->set_linestyle(lineStyle);
        style->set_polystyle(polyStyle);

        PairPtr pair = factory->CreatePair();
        pair->set_styleselector(style);
        pair->set_key(kmldom::STYLESTATE_HIGHLIGHT);

        styleMap->add_pair(pair);
    }

    PlacemarkPtr placemark = factory->CreatePlacemark();
    placemark->set_geometry(linestring);
    placemark->set_styleselector(styleMap);
    placemark->set_visibility(true);

    // Create the timespan
    TimeSpanPtr timeSpan = factory->CreateTimeSpan();
    QDateTime startTime = QDateTime::currentDateTimeUtc().addMSecs(newPlacemarkTime); // FIXME: Make this a function of the true time, preferably gotten from the GPS
    QDateTime endTime = QDateTime::currentDateTimeUtc().addMSecs(newPlacemarkTime);
    timeSpan->set_begin(startTime.toString(dateTimeFormat).toStdString());
    timeSpan->set_end(endTime.toString(dateTimeFormat).toStdString());

    // Set the name
    QDateTime trackTime = QDateTime::currentDateTimeUtc().addMSecs(newPlacemarkTime); // FIXME: Make it a function of the realtime preferably gotten from the GPS
    placemark->set_name(trackTime.toString(dateTimeFormat).toStdString());

    // Add a nice description to the track placemark
    placemark->set_description(informationString.toStdString());

    // Set the timespan
    placemark->set_timeprimitive(timeSpan);

    return placemark;
}


/**
 * @brief KmlExport::createTimespanPlacemark Creates a timespan placemark, which allows the
 * trajectory to be played forward in time. The placemark also contains pertinent data about
 * the vehicle's state at that timespan
 * @param timestampPoint
 * @param lastPlacemarkTime
 * @param newPlacemarkTime
 * @return Returns the placemark containing the timespan
 */
PlacemarkPtr KmlExport::createTimespanPlacemark(const LLAVCoordinates &timestampPoint, quint32 lastPlacemarkTime, quint32 newPlacemarkTime)
{
    // Create coordinates
    CoordinatesPtr coordinates = factory->CreateCoordinates();
    coordinates->add_latlngalt(timestampPoint.latitude, timestampPoint.longitude, timestampPoint.altitude);

    // Create point, using previous coordinates
    PointPtr point = factory->CreatePoint();
    point->set_extrude(true); // Extrude to ground
    point->set_altitudemode(kmldom::ALTITUDEMODE_ABSOLUTE);
    point->set_coordinates(coordinates);

    // Create the timespan
    TimeSpanPtr timeSpan = factory->CreateTimeSpan();
    QDateTime startTime = QDateTime::currentDateTimeUtc().addMSecs(lastPlacemarkTime); // FIXME: Make it a function of the realtime preferably gotten from the GPS
    QDateTime endTime = QDateTime::currentDateTimeUtc().addMSecs(newPlacemarkTime);
    timeSpan->set_begin(startTime.toString(dateTimeFormat).toStdString());
    timeSpan->set_end(endTime.toString(dateTimeFormat).toStdString());

    // Create an icon style. This arrow icon will be rotated and colored to represent velocity
    AttitudeActual::DataFields attitudeActualData = attitudeActual->getData();
    AirspeedActual::DataFields airspeedActualData = airspeedActual->getData();
    IconStylePtr iconStyle = factory->CreateIconStyle();
    iconStyle->set_color(mapVelocity2Color(airspeedActualData.CalibratedAirspeed));
    iconStyle->set_heading(attitudeActualData.Yaw + 180); //Adding 180 degrees because the arrow art points down, i.e. south.

    // Create a line style. This defines the style for the "legs" connecting the points to the ground.
    LineStylePtr lineStyle = factory->CreateLineStyle();
    lineStyle->set_color(mapVelocity2Color(timestampPoint.groundspeed));

    // Link the style to the icon
    StylePtr style = factory->CreateStyle();
    style->set_linestyle(lineStyle);
    style->set_iconstyle(iconStyle);

    // Generate the placemark with all above attributes
    PlacemarkPtr placemark = factory->CreatePlacemark();
    placemark->set_geometry(point);
    placemark->set_timeprimitive(timeSpan);
    placemark->set_name(QString("%1").arg(timeStamp / 1000.0).toStdString());
    placemark->set_visibility(true);

    // Set the placemark to use the custom rotated arrow style
    placemark->set_styleurl("#directiveArrowStyle");
    placemark->set_styleselector(style);

    // Add a nice description to the placemark
    placemark->set_description(informationString.toStdString());

    return placemark;
}


/**
 * @brief KmlExport::mapVelocity2Color Maps a velocity magnitude onto a color.
 * @param velocity Vehicle velocity in [m/s]
 * @param alpha Transparency. If no value provided, color is fully opaque
 * @return Returns the RGBA color
 */
kmlbase::Color32 KmlExport::mapVelocity2Color(double velocity, quint8 alpha)
{
    quint8 colorMapIdx = fmin(fabs(velocity/maxVelocity), 1) * 255;
    quint8 r = round(ColorMap_Jet[colorMapIdx][0]*255); // Colormap is in [0,1], so it needs to be scaled to [0,255]
    quint8 g = round(ColorMap_Jet[colorMapIdx][1]*255);
    quint8 b = round(ColorMap_Jet[colorMapIdx][2]*255);

    return kmlbase::Color32(alpha, b, g, r);
}


/**
 * @brief KmlExport::positionActualUpdated Triggers on PositionActual UAVO
 * update. Converts position to latitude-longitude-altitude and then
 * creates new placemarks.
 * @param obj Unused
 */
void KmlExport::positionActualUpdated(UAVObject *obj)
{
    Q_UNUSED(obj);

    // Only export positional data if the home location has been set.
    if (homeLocationData.Set == HomeLocation::SET_FALSE)
        return;

    // Only plot positional data if we have a lock
    if (gpsPositionData.Status != GPSPosition::STATUS_FIX2D &&
            gpsPositionData.Status != GPSPosition::STATUS_FIX3D)
        return;

    AirspeedActual::DataFields airspeedActualData = airspeedActual->getData();
    PositionActual::DataFields positionActualData = positionActual->getData();
    VelocityActual::DataFields velocityActualData = velocityActual->getData();

    LLAVCoordinates newPoint;

    // Convert NED data to LLA data
    double homeLLA[3]={homeLocationData.Latitude/1e7, homeLocationData.Longitude/1e7, homeLocationData.Altitude};
    double NED[3]={positionActualData.North, positionActualData.East, positionActualData.Down};
    double LLA[3];
    Utils::CoordinateConversions().NED2LLA_HomeLLA(homeLLA, NED, LLA);

    // Generate new placemark
    newPoint.latitude = LLA[0];
    newPoint.longitude = LLA[1];
    newPoint.altitude = LLA[2];
    newPoint.groundspeed = sqrt(velocityActualData.North*velocityActualData.North + velocityActualData.East*velocityActualData.East);

    // Update UAV info string
    informationString.clear();
    informationString.append(QString("Latitude: %1 deg\nLongitude: %2 deg\nAltitude: %3 m\nAirspeed: %4 m/s\nGroundspeed: %5 m/s\n").arg(newPoint.latitude)
                             .arg(newPoint.longitude).arg(newPoint.altitude).arg(airspeedActualData.CalibratedAirspeed).arg(newPoint.groundspeed));

    // In case this is the first time through, copy data and exit
    static bool firstPoint;

    if (firstPoint == false) {
        oldPoint.latitude = newPoint.latitude;
        oldPoint.longitude = newPoint.longitude;
        oldPoint.altitude = newPoint.altitude;
        oldPoint.groundspeed = newPoint.groundspeed;

        firstPoint = true;
        return;
    }

    // Create wall axes
    for (int i=0; i<numberOfWallAxes; i++){
        wallAxes[i]->add_latlngalt(newPoint.latitude, newPoint.longitude, i*wallAxesSeparation + homeLocationData.Altitude);
    }

    // Create colored tracks and add to the KML document
    PlacemarkPtr newPlacemark = CreateLineStringPlacemark(oldPoint, newPoint, timeStamp);
    trackFolder->add_feature(newPlacemark);

    // Every 2 seconds generate a time stamp
    if (timeStamp - lastPlacemarkTime > 2000) {

        PlacemarkPtr newPlacemarkTimestamp = createTimespanPlacemark(newPoint, lastPlacemarkTime, timeStamp);
        timestampFolder->add_feature(newPlacemarkTimestamp);
        lastPlacemarkTime = timeStamp;
    }

    // Copy newPoint to oldPoint
    oldPoint.latitude = newPoint.latitude;
    oldPoint.longitude = newPoint.longitude;
    oldPoint.altitude = newPoint.altitude;
    oldPoint.groundspeed = newPoint.groundspeed;

}

void KmlExport::homeLocationUpdated(UAVObject *obj)
{
    Q_UNUSED(obj);
    homeLocationData = homeLocation->getData();
}

void KmlExport::gpsPositionUpdated(UAVObject *obj)
{
    Q_UNUSED(obj);
    gpsPositionData = gpsPosition->getData();
}

/**
 * @}
 * @}
 */
