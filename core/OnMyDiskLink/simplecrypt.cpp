/*
Copyright (c) 2011, Andre Somers
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
	* Redistributions of source code must retain the above copyright
	  notice, this list of conditions and the following disclaimer.
	* Redistributions in binary form must reproduce the above copyright
	  notice, this list of conditions and the following disclaimer in the
	  documentation and/or other materials provided with the distribution.
	* Neither the name of the Rathenau Instituut, Andre Somers nor the
	  names of its contributors may be used to endorse or promote products
	  derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL ANDRE SOMERS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR #######; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
#include "simplecrypt.h"
#include <QByteArray>
#include <QtDebug>
#include <QtGlobal>
#include <QDateTime>
#include <QCryptographicHash>
#include <QDataStream>
#if QT_VERSION >= 0x050F00
#include <QRandomGenerator>
#endif

quint64 SimpleCrypt::defaultKey = 0x064105012;

SimpleCrypt::SimpleCrypt():
	m_key(0),
	m_compressionMode(CompressionAuto),
	m_protectionMode(ProtectionChecksum),
	m_lastError(ErrorNoError)
{
    init();
}

SimpleCrypt::SimpleCrypt(quint64 key):
    m_key(key != 0 ? key : defaultKey),
	m_compressionMode(CompressionAuto),
	m_protectionMode(ProtectionChecksum),
	m_lastError(ErrorNoError)
{
    init();
    splitKey();
}

void SimpleCrypt::setKey(quint64 key)
{
	m_key = key;
	splitKey();
}

void SimpleCrypt::splitKey()
{
	m_keyParts.clear();
	m_keyParts.resize(8);
	for (int i=0;i<8;i++) {
		quint64 part = m_key;
		for (int j=i; j>0; j--)
			part = part >> 8;
		part = part & 0xff;
		m_keyParts[i] = static_cast<char>(part);
    }
}

void SimpleCrypt::init()
{
    #if QT_VERSION < 0x050F00
    qsrand(uint(QDateTime::currentMSecsSinceEpoch() & 0xFFFF));
    #endif
}

QByteArray SimpleCrypt::encryptToByteArray(const QString& plaintext)
{
	QByteArray plaintextArray = plaintext.toUtf8();
	return encryptToByteArray(plaintextArray);
}

QByteArray SimpleCrypt::encryptToByteArray(QByteArray plaintext)
{
	if (m_keyParts.isEmpty()) {
		qWarning() << "No key set.";
		m_lastError = ErrorNoKeySet;
		return QByteArray();
	}


	QByteArray ba = plaintext;

	CryptoFlags flags = CryptoFlagNone;
	if (m_compressionMode == CompressionAlways) {
		ba = qCompress(ba, 9); //maximum compression
		flags |= CryptoFlagCompression;
	} else if (m_compressionMode == CompressionAuto) {
		QByteArray compressed = qCompress(ba, 9);
		if (compressed.count() < ba.count()) {
			ba = compressed;
			flags |= CryptoFlagCompression;
		}
	}

	QByteArray integrityProtection;
	if (m_protectionMode == ProtectionChecksum) {
		flags |= CryptoFlagChecksum;
		QDataStream s(&integrityProtection, QIODevice::WriteOnly);
		s << qChecksum(ba.constData(), ba.length());
	} else if (m_protectionMode == ProtectionHash) {
		flags |= CryptoFlagHash;
		QCryptographicHash hash(QCryptographicHash::Sha1);
		hash.addData(ba);

		integrityProtection += hash.result();
	}

	//prepend a random char to the string
    char randomChar = char(rand() & 0xFF);
	ba = randomChar + integrityProtection + ba;

	int pos(0);
	char lastChar(0);

	int cnt = ba.count();

	while (pos < cnt) {
		ba[pos] = ba.at(pos) ^ m_keyParts.at(pos % 8) ^ lastChar;
		lastChar = ba.at(pos);
		++pos;
	}

	QByteArray resultArray;
	resultArray.append(char(0x03));  //version for future updates to algorithm
	resultArray.append(char(flags)); //encryption flags
	resultArray.append(ba);

	m_lastError = ErrorNoError;
	return resultArray;
}

QString SimpleCrypt::encryptToString(const QString& plaintext)
{
	QByteArray plaintextArray = plaintext.toUtf8();
	QByteArray cypher = encryptToByteArray(plaintextArray);
    QString cypherString = QString::fromLatin1(cypher.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
	return cypherString;
}

QString SimpleCrypt::encryptToString(QByteArray plaintext)
{
	QByteArray cypher = encryptToByteArray(plaintext);
    QString cypherString = QString::fromLatin1(cypher.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
	return cypherString;
}

QString SimpleCrypt::decryptToString(const QString &cyphertext)
{
    QByteArray cyphertextArray = QByteArray::fromBase64(cyphertext.toLatin1());
	QByteArray plaintextArray = decryptToByteArray(cyphertextArray);
	QString plaintext = QString::fromUtf8(plaintextArray, plaintextArray.size());

	return plaintext;
}

QString SimpleCrypt::decryptUrlEncodedToString(const QString &cyphertext)
{
    QByteArray cyphertextArray = QByteArray::fromBase64(cyphertext.toLatin1(), QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    QByteArray plaintextArray = decryptToByteArray(cyphertextArray);
    QString plaintext = QString::fromUtf8(plaintextArray, plaintextArray.size());

    return plaintext;
}


QString SimpleCrypt::decryptToString(QByteArray cypher)
{
	QByteArray ba = decryptToByteArray(cypher);
	QString plaintext = QString::fromUtf8(ba, ba.size());

	return plaintext;
}

QByteArray SimpleCrypt::decryptToByteArray(const QString& cyphertext)
{
    QByteArray cyphertextArray = QByteArray::fromBase64(cyphertext.toLatin1(), QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
	QByteArray ba = decryptToByteArray(cyphertextArray);

	return ba;
}

QByteArray SimpleCrypt::decryptToByteArray(QByteArray cypher)
{
	if (m_keyParts.isEmpty()) {
		qWarning() << "No key set.";
		m_lastError = ErrorNoKeySet;
		return QByteArray();
	}

	QByteArray ba = cypher;

	if( cypher.count() < 3 )
		return QByteArray();

	char version = ba.at(0);

	if (version !=3) {  //we only work with version 3
		m_lastError = ErrorUnknownVersion;
		qWarning() << "Invalid version or not a cyphertext.";
		return QByteArray();
	}

	CryptoFlags flags = CryptoFlags(ba.at(1));

	ba = ba.mid(2);
	int pos(0);
	int cnt(ba.count());
	char lastChar = 0;

	while (pos < cnt) {
		char currentChar = ba[pos];
		ba[pos] = ba.at(pos) ^ lastChar ^ m_keyParts.at(pos % 8);
		lastChar = currentChar;
		++pos;
	}

	ba = ba.mid(1); //chop off the random number at the start

	bool integrityOk(true);
	if (flags.testFlag(CryptoFlagChecksum)) {
		if (ba.length() < 2) {
			m_lastError = ErrorIntegrityFailed;
			return QByteArray();
		}
		quint16 storedChecksum;
		{
			QDataStream s(&ba, QIODevice::ReadOnly);
			s >> storedChecksum;
		}
		ba = ba.mid(2);
		quint16 checksum = qChecksum(ba.constData(), ba.length());
		integrityOk = (checksum == storedChecksum);
	} else if (flags.testFlag(CryptoFlagHash)) {
		if (ba.length() < 20) {
			m_lastError = ErrorIntegrityFailed;
			return QByteArray();
		}
		QByteArray storedHash = ba.left(20);
		ba = ba.mid(20);
		QCryptographicHash hash(QCryptographicHash::Sha1);
		hash.addData(ba);
		integrityOk = (hash.result() == storedHash);
	}

	if (!integrityOk) {
		m_lastError = ErrorIntegrityFailed;
		return QByteArray();
	}

	if (flags.testFlag(CryptoFlagCompression))
		ba = qUncompress(ba);

	m_lastError = ErrorNoError;
	return ba;
}

quint32 SimpleCrypt::rand()
{
    #if QT_VERSION >= 0x050F00
    return QRandomGenerator::system()->generate();
    #else
    return qrand();
    #endif
}

QString SimpleCrypt::stringToHashString(const QString& str) {

    quint64 numericHash = (quint64) stringToHash(str);
    // Convert the integer to a hexadecimal string
    QString hexString = QString::number(numericHash, 16); // Use base 16 for hexadecimal
    return hexString;

}

QString SimpleCrypt::convertToHexString(const QString &numericString) {
    // Convert the numeric string to a 64-bit integer
    bool ok;
    qint64 numericValue = numericString.toLongLong(&ok);

    // Check if the conversion was successful
    if (!ok) {
        qWarning() << "Failed to convert string to integer:" << numericString;
        return QString();
    }

    // Convert the integer to a hexadecimal string
    QString hexString = QString::number(numericValue, 16); // Use base 16 for hexadecimal
    return hexString;
}


qint64 SimpleCrypt::stringToHash(const QString &str)
{
    QByteArray hash = QCryptographicHash::hash(str.toUtf8(), QCryptographicHash::Md5);
    Q_ASSERT(hash.size() == 16);
    QDataStream stream(hash);
    quint64 a, b;
    stream >> a >> b;
    return a ^ b;
}

QString SimpleCrypt::randomKey(int length)
{
   const QString alphanumericCharacters("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789");
   QString key;
   int i = 0;
   while( i < length)
   {
       int index = SimpleCrypt::rand() % alphanumericCharacters.length();
       QChar nextChar = alphanumericCharacters.at(index);
       if (key.indexOf(nextChar) == -1) {
         key.append(nextChar);
         i++;
       }
   }
   return key;
}

QString SimpleCrypt::randomNumKey(int length)
{
   const QString numericCharacters("0123456789");
   QString key;
   int i = 0;
   while( i < length)
   {
       int index = SimpleCrypt::rand() % numericCharacters.length();
       QChar nextChar = numericCharacters.at(index);
       if (key.indexOf(nextChar) == -1) {
         key.append(nextChar);
         i++;
       }
   }
   return key;
}
