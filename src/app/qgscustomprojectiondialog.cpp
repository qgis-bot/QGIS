/***************************************************************************
                          qgscustomprojectiondialog.cpp

                             -------------------
    begin                : 2005
    copyright            : (C) 2005 by Tim Sutton
    email                : tim@linfiniti.com
***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "qgscustomprojectiondialog.h"

//qgis includes
#include "qgis.h" //<--magick numbers
#include "qgisapp.h" //<--theme icons
#include "qgsapplication.h"
#include "qgslogger.h"
#include "qgsprojectionselectiondialog.h"
#include "qgssettings.h"
#include "qgssqliteutils.h"
#include "qgsgui.h"

//qt includes
#include <QFileInfo>
#include <QMessageBox>
#include <QLocale>

//stdc++ includes
#include <fstream>
#include <sqlite3.h>

//proj4 includes
#if PROJ_VERSION_MAJOR>=6
#include "qgsprojutils.h"
#include <proj.h>
#else
#include <proj_api.h>
#endif

QgsCustomProjectionDialog::QgsCustomProjectionDialog( QWidget *parent, Qt::WindowFlags fl )
  : QDialog( parent, fl )
{
  setupUi( this );
  QgsGui::enableAutoGeometryRestore( this );

  connect( pbnCalculate, &QPushButton::clicked, this, &QgsCustomProjectionDialog::pbnCalculate_clicked );
  connect( pbnAdd, &QPushButton::clicked, this, &QgsCustomProjectionDialog::pbnAdd_clicked );
  connect( pbnRemove, &QPushButton::clicked, this, &QgsCustomProjectionDialog::pbnRemove_clicked );
  connect( pbnCopyCRS, &QPushButton::clicked, this, &QgsCustomProjectionDialog::pbnCopyCRS_clicked );
  connect( leNameList, &QTreeWidget::currentItemChanged, this, &QgsCustomProjectionDialog::leNameList_currentItemChanged );
  connect( buttonBox, &QDialogButtonBox::accepted, this, &QgsCustomProjectionDialog::buttonBox_accepted );
  connect( buttonBox, &QDialogButtonBox::helpRequested, this, &QgsCustomProjectionDialog::showHelp );
  connect( mButtonValidate, &QPushButton::clicked, this, &QgsCustomProjectionDialog::validateCurrent );

  leNameList->setSelectionMode( QAbstractItemView::ExtendedSelection );

  // user database is created at QGIS startup in QgisApp::createDB
  // we just check whether there is our database [MD]
  QFileInfo fileInfo;
  fileInfo.setFile( QgsApplication::qgisSettingsDirPath() );
  if ( !fileInfo.exists() )
  {
    QgsDebugMsg( QStringLiteral( "The qgis.db does not exist" ) );
  }

  populateList();
  if ( !mCustomCRSnames.empty() )
  {
    whileBlocking( leName )->setText( mCustomCRSnames[0] );
    whileBlocking( teParameters )->setPlainText( mCustomCRSparameters[0] );
    leNameList->setCurrentItem( leNameList->topLevelItem( 0 ) );
  }

  leNameList->hideColumn( QgisCrsIdColumn );

  connect( leName, &QLineEdit::textChanged, this, &QgsCustomProjectionDialog::updateListFromCurrentItem );
  connect( teParameters, &QPlainTextEdit::textChanged, this, &QgsCustomProjectionDialog::updateListFromCurrentItem );
}

void QgsCustomProjectionDialog::populateList()
{
  //Setup connection to the existing custom CRS database:
  sqlite3_database_unique_ptr database;
  sqlite3_statement_unique_ptr preparedStatement;
  //check the db is available
  int result = database.open_v2( QgsApplication::qgisUserDatabaseFilePath(), SQLITE_OPEN_READONLY, nullptr );
  if ( result != SQLITE_OK )
  {
    QgsDebugMsg( QStringLiteral( "Can't open database: %1" ).arg( database.errorMessage() ) );
    // XXX This will likely never happen since on open, sqlite creates the
    //     database if it does not exist.
    Q_ASSERT( result == SQLITE_OK );
  }
  QString sql = QStringLiteral( "select srs_id,description,parameters from tbl_srs" );
  QgsDebugMsgLevel( QStringLiteral( "Query to populate existing list:%1" ).arg( sql ), 4 );
  preparedStatement = database.prepare( sql, result );
  if ( result == SQLITE_OK )
  {
    QgsCoordinateReferenceSystem crs;
    while ( preparedStatement.step() == SQLITE_ROW )
    {
      QString id = preparedStatement.columnAsText( 0 );
      QString name = preparedStatement.columnAsText( 1 );
      QString parameters = preparedStatement.columnAsText( 2 );

      crs.createFromProj4( parameters );
      mExistingCRSnames[id] = name;
      mExistingCRSparameters[id] = crs.toProj4();

      QTreeWidgetItem *newItem = new QTreeWidgetItem( leNameList, QStringList() );
      newItem->setText( QgisCrsNameColumn, name );
      newItem->setText( QgisCrsIdColumn, id );
      newItem->setText( QgisCrsParametersColumn, crs.toProj4() );
    }
  }
  else
  {
    QgsDebugMsg( QStringLiteral( "Populate list query failed: %1" ).arg( sql ) );
  }
  preparedStatement.reset();

  leNameList->sortByColumn( QgisCrsNameColumn, Qt::AscendingOrder );

  QTreeWidgetItemIterator it( leNameList );
  while ( *it )
  {
    QString id = ( *it )->text( QgisCrsIdColumn );
    mCustomCRSids.push_back( id );
    mCustomCRSnames.push_back( mExistingCRSnames[id] );
    mCustomCRSparameters.push_back( mExistingCRSparameters[id] );
    it++;
  }
}

bool  QgsCustomProjectionDialog::deleteCrs( const QString &id )
{
  sqlite3_database_unique_ptr database;

  QString sql = "delete from tbl_srs where srs_id=" + QgsSqliteUtils::quotedString( id );
  QgsDebugMsgLevel( sql, 4 );
  //check the db is available
  int result = database.open( QgsApplication::qgisUserDatabaseFilePath() );
  if ( result != SQLITE_OK )
  {
    QgsDebugMsg( QStringLiteral( "Can't open database: %1 \n please notify  QGIS developers of this error \n %2 (file name) " ).arg( database.errorMessage(),
                 QgsApplication::qgisUserDatabaseFilePath() ) );
    // XXX This will likely never happen since on open, sqlite creates the
    //     database if it does not exist.
    Q_ASSERT( result == SQLITE_OK );
  }
  {
    sqlite3_statement_unique_ptr preparedStatement = database.prepare( sql, result );
    if ( result != SQLITE_OK || preparedStatement.step() != SQLITE_DONE )
    {
      QgsDebugMsg( QStringLiteral( "failed to remove CRS from database in custom projection dialog: %1 [%2]" ).arg( sql, database.errorMessage() ) );
    }
  }

  QgsCoordinateReferenceSystem::invalidateCache();
  QgsCoordinateTransform::invalidateCache();

  return result == SQLITE_OK;
}

void  QgsCustomProjectionDialog::insertProjection( const QString &projectionAcronym )
{
  sqlite3_database_unique_ptr database;
  sqlite3_database_unique_ptr srsDatabase;
  QString sql;
  //check the db is available
  int result = database.open( QgsApplication::qgisUserDatabaseFilePath() );
  if ( result != SQLITE_OK )
  {
    QgsDebugMsg( QStringLiteral( "Can't open database: %1 \n please notify  QGIS developers of this error \n %2 (file name) " ).arg( database.errorMessage(),
                 QgsApplication::qgisUserDatabaseFilePath() ) );
    // XXX This will likely never happen since on open, sqlite creates the
    //     database if it does not exist.
    Q_ASSERT( result == SQLITE_OK );
  }
  int srsResult = srsDatabase.open( QgsApplication::srsDatabaseFilePath() );
  if ( result != SQLITE_OK )
  {
    QgsDebugMsg( QStringLiteral( "Can't open database %1 [%2]" ).arg( QgsApplication::srsDatabaseFilePath(),
                 srsDatabase.errorMessage() ) );
  }
  else
  {
    // Set up the query to retrieve the projection information needed to populate the PROJECTION list
    QString srsSql = "select acronym,name,notes,parameters from tbl_projection where acronym=" + QgsSqliteUtils::quotedString( projectionAcronym );

    sqlite3_statement_unique_ptr srsPreparedStatement = srsDatabase.prepare( srsSql, srsResult );
    if ( srsResult == SQLITE_OK )
    {
      if ( srsPreparedStatement.step() == SQLITE_ROW )
      {
        QgsDebugMsgLevel( QStringLiteral( "Trying to insert projection" ), 4 );
        // We have the result from system srs.db. Now insert into user db.
        sql = "insert into tbl_projection(acronym,name,notes,parameters) values ("
              + QgsSqliteUtils::quotedString( srsPreparedStatement.columnAsText( 0 ) )
              + ',' + QgsSqliteUtils::quotedString( srsPreparedStatement.columnAsText( 1 ) )
              + ',' + QgsSqliteUtils::quotedString( srsPreparedStatement.columnAsText( 2 ) )
              + ',' + QgsSqliteUtils::quotedString( srsPreparedStatement.columnAsText( 3 ) )
              + ')';
        sqlite3_statement_unique_ptr preparedStatement = database.prepare( sql, result );
        if ( result != SQLITE_OK || preparedStatement.step() != SQLITE_DONE )
        {
          QgsDebugMsg( QStringLiteral( "Update or insert failed in custom projection dialog: %1 [%2]" ).arg( sql, database.errorMessage() ) );
        }
      }
    }
    else
    {
      QgsDebugMsg( QStringLiteral( "prepare failed: %1 [%2]" ).arg( srsSql, srsDatabase.errorMessage() ) );
    }
  }
}

bool QgsCustomProjectionDialog::saveCrs( QgsCoordinateReferenceSystem parameters, const QString &name, const QString &existingId, bool newEntry )
{
  QString id = existingId;
  QString sql;
  int returnId;
  QString projectionAcronym = parameters.projectionAcronym();
  QString ellipsoidAcronym = parameters.ellipsoidAcronym();
  QgsDebugMsgLevel( QStringLiteral( "Saving a CRS:%1, %2, %3" ).arg( name, parameters.toProj4() ).arg( newEntry ), 4 );
  if ( newEntry )
  {
    returnId = parameters.saveAsUserCrs( name );
    if ( returnId == -1 )
      return false;
    else
      id = QString::number( returnId );
  }
  else
  {
    sql = "update tbl_srs set description="
          + QgsSqliteUtils::quotedString( name )
          + ",projection_acronym=" + ( !projectionAcronym.isEmpty() ? QgsSqliteUtils::quotedString( projectionAcronym ) : QStringLiteral( "''" ) )
          + ",ellipsoid_acronym=" + ( !ellipsoidAcronym.isEmpty() ? QgsSqliteUtils::quotedString( ellipsoidAcronym ) : QStringLiteral( "''" ) )
          + ",parameters=" + QgsSqliteUtils::quotedString( parameters.toProj4() )
          + ",is_geo=0" // <--shamelessly hard coded for now
          + ",wkt=" + QgsSqliteUtils::quotedString( parameters.toWkt() )
          + " where srs_id=" + QgsSqliteUtils::quotedString( id )
          ;
    QgsDebugMsgLevel( sql, 4 );
    sqlite3_database_unique_ptr database;
    //check if the db is available
    int result = database.open( QgsApplication::qgisUserDatabaseFilePath() );
    if ( result != SQLITE_OK )
    {
      QgsDebugMsg( QStringLiteral( "Can't open database: %1 \n please notify  QGIS developers of this error \n %2 (file name) " ).arg( database.errorMessage(),
                   QgsApplication::qgisUserDatabaseFilePath() ) );
      // XXX This will likely never happen since on open, sqlite creates the
      //     database if it does not exist.
      Q_ASSERT( result == SQLITE_OK );
    }
    sqlite3_statement_unique_ptr preparedStatement = database.prepare( sql, result );
    if ( result != SQLITE_OK || preparedStatement.step() != SQLITE_DONE )
    {
      QgsDebugMsg( QStringLiteral( "failed to write to database in custom projection dialog: %1 [%2]" ).arg( sql, database.errorMessage() ) );
    }

    preparedStatement.reset();
    if ( result != SQLITE_OK )
      return false;
  }
  mExistingCRSparameters[id] = parameters.toProj4();
  mExistingCRSnames[id] = name;

  QgsCoordinateReferenceSystem::invalidateCache();
  QgsCoordinateTransform::invalidateCache();

  // If we have a projection acronym not in the user db previously, add it.
  // This is a must, or else we can't select it from the vw_srs table.
  // Actually, add it always and let the SQL PRIMARY KEY remove duplicates.
  insertProjection( projectionAcronym );

  return true;
}


void QgsCustomProjectionDialog::pbnAdd_clicked()
{
  QString name = tr( "new CRS" );
  QString id;
  QgsCoordinateReferenceSystem parameters;

  QTreeWidgetItem *newItem = new QTreeWidgetItem( leNameList, QStringList() );

  newItem->setText( QgisCrsNameColumn, name );
  newItem->setText( QgisCrsIdColumn, id );
  newItem->setText( QgisCrsParametersColumn, parameters.toProj4() );
  mCustomCRSnames.push_back( name );
  mCustomCRSids.push_back( id );
  mCustomCRSparameters.push_back( parameters.toProj4() );
  leNameList->setCurrentItem( newItem );
  leName->selectAll();
  leName->setFocus();
}

void QgsCustomProjectionDialog::pbnRemove_clicked()
{
  const QModelIndexList selection = leNameList->selectionModel()->selectedRows();
  if ( selection.empty() )
    return;

  // make sure the user really wants to delete these definitions
  if ( QMessageBox::No == QMessageBox::question( this, tr( "Delete Projections" ),
       tr( "Are you sure you want to delete %n projections(s)?", "number of rows", selection.size() ),
       QMessageBox::Yes | QMessageBox::No ) )
    return;

  std::vector< int > selectedRows;
  selectedRows.reserve( selection.size() );
  for ( const QModelIndex &index : selection )
    selectedRows.emplace_back( index.row() );

  //sort rows in reverse order
  std::sort( selectedRows.begin(), selectedRows.end(), std::greater< int >() );
  for ( const int row : selectedRows )
  {
    if ( row < 0 )
    {
      // shouldn't happen?
      continue;
    }
    delete leNameList->takeTopLevelItem( row );
    if ( !mCustomCRSids[row].isEmpty() )
    {
      mDeletedCRSs.push_back( mCustomCRSids[row] );
    }
    mCustomCRSids.erase( mCustomCRSids.begin() + row );
    mCustomCRSnames.erase( mCustomCRSnames.begin() + row );
    mCustomCRSparameters.erase( mCustomCRSparameters.begin() + row );
  }
}

void QgsCustomProjectionDialog::leNameList_currentItemChanged( QTreeWidgetItem *current, QTreeWidgetItem *previous )
{
  //Store the modifications made to the current element before moving on
  int currentIndex, previousIndex;
  if ( previous )
  {
    previousIndex = leNameList->indexOfTopLevelItem( previous );
    mCustomCRSnames[previousIndex] = leName->text();
    mCustomCRSparameters[previousIndex] = teParameters->toPlainText();
    previous->setText( QgisCrsNameColumn, leName->text() );
    previous->setText( QgisCrsParametersColumn, teParameters->toPlainText() );
  }
  if ( current )
  {
    currentIndex = leNameList->indexOfTopLevelItem( current );
    whileBlocking( leName )->setText( mCustomCRSnames[currentIndex] );
    whileBlocking( teParameters )->setPlainText( current->text( QgisCrsParametersColumn ) );
  }
  else
  {
    //Can happen that current is null, for example if we just deleted the last element
    leName->clear();
    teParameters->clear();
    return;
  }
}

void QgsCustomProjectionDialog::pbnCopyCRS_clicked()
{
  std::unique_ptr< QgsProjectionSelectionDialog > selector = qgis::make_unique< QgsProjectionSelectionDialog >( this );
  if ( selector->exec() )
  {
    QgsCoordinateReferenceSystem srs = selector->crs();
    if ( leNameList->topLevelItemCount() == 0 )
    {
      pbnAdd_clicked();
    }
    teParameters->setPlainText( srs.toProj4() );
    mCustomCRSparameters[leNameList->currentIndex().row()] = srs.toProj4();
    leNameList->currentItem()->setText( QgisCrsParametersColumn, srs.toProj4() );

  }
}

void QgsCustomProjectionDialog::buttonBox_accepted()
{
  //Update the current CRS:
  int i = leNameList->currentIndex().row();
  if ( i != -1 )
  {
    mCustomCRSnames[i] = leName->text();
    mCustomCRSparameters[i] = teParameters->toPlainText();
  }

  QgsDebugMsgLevel( QStringLiteral( "We save the modified CRS." ), 4 );

  //Check if all CRS are valid:
  QgsCoordinateReferenceSystem CRS;
  for ( int i = 0; i < mCustomCRSids.size(); ++i )
  {
    CRS.createFromProj4( mCustomCRSparameters[i] );
    if ( !CRS.isValid() )
    {
      // auto select the invalid CRS row
      for ( int row = 0; row < leNameList->model()->rowCount(); ++row )
      {
        if ( leNameList->model()->data( leNameList->model()->index( row, QgisCrsNameColumn ) ).toString() == mCustomCRSnames[i]
             && leNameList->model()->data( leNameList->model()->index( row, QgisCrsParametersColumn ) ).toString() == mCustomCRSparameters[i] )
        {
          //leNameList_currentItemChanged( leNameList->invisibleRootItem()->child( row ), leNameList->currentItem() );
          leNameList->setCurrentItem( leNameList->invisibleRootItem()->child( row ) );
          //leNameList->selectionModel()->select( leNameList->model()->index( row, 0 ), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows );
          break;
        }
      }

      QMessageBox::warning( this, tr( "Custom Coordinate Reference System" ),
                            tr( "The proj4 definition of '%1' is not valid." ).arg( mCustomCRSnames[i] ) );
      return;
    }
  }
  //Modify the CRS changed:
  bool saveSuccess = true;
  for ( int i = 0; i < mCustomCRSids.size(); ++i )
  {
    CRS.createFromProj4( mCustomCRSparameters[i] );
    //Test if we just added this CRS (if it has no existing ID)
    if ( mCustomCRSids[i].isEmpty() )
    {
      saveSuccess &= saveCrs( CRS, mCustomCRSnames[i], QString(), true );
    }
    else
    {
      if ( mExistingCRSnames[mCustomCRSids[i]] != mCustomCRSnames[i] || mExistingCRSparameters[mCustomCRSids[i]] != mCustomCRSparameters[i] )
      {
        saveSuccess &= saveCrs( CRS, mCustomCRSnames[i], mCustomCRSids[i], false );
      }
    }
    if ( ! saveSuccess )
    {
      QgsDebugMsg( QStringLiteral( "Error when saving CRS '%1'" ).arg( mCustomCRSnames[i] ) );
    }
  }
  QgsDebugMsgLevel( QStringLiteral( "We remove the deleted CRS." ), 4 );
  for ( int i = 0; i < mDeletedCRSs.size(); ++i )
  {
    saveSuccess &= deleteCrs( mDeletedCRSs[i] );
    if ( ! saveSuccess )
    {
      QgsDebugMsg( QStringLiteral( "Problem for layer '%1'" ).arg( mCustomCRSparameters[i] ) );
    }
  }
  if ( saveSuccess )
  {
    accept();
  }
}

void QgsCustomProjectionDialog::updateListFromCurrentItem()
{
  QTreeWidgetItem *item = leNameList->currentItem();
  if ( !item )
    return;

  int currentIndex = leNameList->indexOfTopLevelItem( item );
  if ( currentIndex < 0 )
    return;

  mCustomCRSnames[currentIndex] = leName->text();
  mCustomCRSparameters[currentIndex] = teParameters->toPlainText();
  item->setText( QgisCrsNameColumn, leName->text() );
  item->setText( QgisCrsParametersColumn, teParameters->toPlainText() );
}

#if PROJ_VERSION_MAJOR>=6
static void proj_collecting_logger( void *user_data, int /*level*/, const char *message )
{
  QStringList *dest = reinterpret_cast< QStringList * >( user_data );
  QString messageString( message );
  messageString.replace( QStringLiteral( "internal_proj_create: " ), QString() );
  dest->append( messageString );
}

#endif

void QgsCustomProjectionDialog::validateCurrent()
{
  const QString projDef = teParameters->toPlainText();

#if PROJ_VERSION_MAJOR>=6
  PJ_CONTEXT *context = proj_context_create();

  QStringList projErrors;
  proj_log_func( context, &projErrors, proj_collecting_logger );

  const QString projCrsString = projDef + ( projDef.contains( QStringLiteral( "+type=crs" ) ) ? QString() : QStringLiteral( " +type=crs" ) );
  QgsProjUtils::proj_pj_unique_ptr crs( proj_create( context, projCrsString.toLatin1().constData() ) );
  if ( crs )
  {
    QMessageBox::information( this, tr( "Custom Coordinate Reference System" ),
                              tr( "This proj projection definition is valid." ) );
  }
  else
  {
    QMessageBox::warning( this, tr( "Custom Coordinate Reference System" ),
                          tr( "This proj projection definition is not valid:" ) + QStringLiteral( "\n\n" ) + projErrors.join( '\n' ) );
  }

  // reset logger to terminal output
  proj_log_func( context, nullptr, nullptr );
  proj_context_destroy( context );
  context = nullptr;
#else
  projCtx pContext = pj_ctx_alloc();
  projPJ proj = pj_init_plus_ctx( pContext, projDef.toLocal8Bit().data() );

  if ( proj )
  {
    QMessageBox::information( this, tr( "Custom Coordinate Reference System" ),
                              tr( "This proj projection definition is valid." ) );
  }
  else
  {
    QMessageBox::warning( this, tr( "Custom Coordinate Reference System" ),
                          tr( "This proj projection definition is not valid" ) );
  }

  pj_free( proj );
  pj_ctx_free( pContext );
#endif
}

void QgsCustomProjectionDialog::pbnCalculate_clicked()
{
  // We must check the prj def is valid!
#if PROJ_VERSION_MAJOR>=6
  PJ_CONTEXT *pContext = QgsProjContext::get();
  QString projDef = teParameters->toPlainText();
  QgsDebugMsgLevel( QStringLiteral( "Proj: %1" ).arg( projDef ), 3 );
#else
  projCtx pContext = pj_ctx_alloc();
  projPJ proj = pj_init_plus_ctx( pContext, teParameters->toPlainText().toLocal8Bit().data() );
  QgsDebugMsgLevel( QStringLiteral( "Proj: %1" ).arg( teParameters->toPlainText() ), 3 );

  if ( !proj )
  {
    QMessageBox::warning( this, tr( "Custom Coordinate Reference System" ),
                          tr( "This proj projection definition is not valid." ) );
    projectedX->clear();
    projectedY->clear();
    pj_free( proj );
    pj_ctx_free( pContext );
    return;

  }
#endif
  // Get the WGS84 coordinates
  bool okN, okE;
  double latitude = northWGS84->text().toDouble( &okN );
  double longitude = eastWGS84->text().toDouble( &okE );

#if PROJ_VERSION_MAJOR<6
  latitude *= DEG_TO_RAD;
  longitude *= DEG_TO_RAD;
#endif

  if ( !okN || !okE )
  {
    QMessageBox::warning( this, tr( "Custom Coordinate Reference System" ),
                          tr( "Northing and Easting must be in decimal form." ) );
    projectedX->clear();
    projectedY->clear();
#if PROJ_VERSION_MAJOR<6
    pj_free( proj );
    pj_ctx_free( pContext );
#endif
    return;
  }

#if PROJ_VERSION_MAJOR < 6
  projPJ wgs84Proj = pj_init_plus_ctx( pContext, geoProj4().data() ); //defined in qgis.h

  if ( !wgs84Proj )
  {
    QMessageBox::critical( this, tr( "Custom Coordinate Reference System" ),
                           tr( "Internal Error (source projection invalid?)" ) );
    projectedX->clear();
    projectedY->clear();
    pj_free( wgs84Proj );
    pj_ctx_free( pContext );
    return;
  }
#endif

#if PROJ_VERSION_MAJOR>=6

  projDef = projDef + ( projDef.contains( QStringLiteral( "+type=crs" ) ) ? QString() : QStringLiteral( " +type=crs" ) );
  QgsProjUtils::proj_pj_unique_ptr res( proj_create_crs_to_crs( pContext, "EPSG:4326", projDef.toUtf8(), nullptr ) );
  if ( !res )
  {
    QMessageBox::warning( this, tr( "Custom Coordinate Reference System" ),
                          tr( "This proj projection definition is not valid." ) );
    projectedX->clear();
    projectedY->clear();
    return;
  }

  // careful -- proj 6 respects CRS axis, so we've got latitude/longitude flowing in, and ....?? coming out?
  proj_trans_generic( res.get(), PJ_FWD,
                      &latitude, sizeof( double ), 1,
                      &longitude, sizeof( double ), 1,
                      nullptr, sizeof( double ), 0,
                      nullptr, sizeof( double ), 0 );
  int projResult = proj_errno( res.get() );
#else
  double z = 0.0;
  int projResult = pj_transform( wgs84Proj, proj, 1, 0, &longitude, &latitude, &z );
#endif
  if ( projResult != 0 )
  {
    projectedX->setText( tr( "Error" ) );
    projectedY->setText( tr( "Error" ) );
#if PROJ_VERSION_MAJOR>=6
    QgsDebugMsg( proj_errno_string( projResult ) );
#else
    QgsDebugMsg( pj_strerrno( projResult ) );
#endif
  }
  else
  {
    QString tmp;

    int precision = 4;
    bool isLatLong = false;

#if PROJ_VERSION_MAJOR>= 6
    isLatLong = QgsProjUtils::usesAngularUnit( projDef );
#else
    isLatLong = pj_is_latlong( proj );
    if ( isLatLong )
    {
      latitude *= RAD_TO_DEG;
      longitude *= RAD_TO_DEG;
    }
#endif
    if ( isLatLong )
    {
      precision = 7;
    }

#if PROJ_VERSION_MAJOR>= 6
    tmp = QLocale().toString( longitude, 'f', precision );
    projectedX->setText( tmp );
    tmp = QLocale().toString( latitude, 'f', precision );
    projectedY->setText( tmp );
#else
    tmp = QLocale().toString( latitude, 'f', precision );
    projectedX->setText( tmp );
    tmp = QLocale().toString( longitude, 'f', precision );
    projectedY->setText( tmp );
#endif
  }

#if PROJ_VERSION_MAJOR<6
  pj_free( proj );
  pj_free( wgs84Proj );
  pj_ctx_free( pContext );
#endif
}

void QgsCustomProjectionDialog::showHelp()
{
  QgsHelp::openHelp( QStringLiteral( "working_with_projections/working_with_projections.html" ) );
}
