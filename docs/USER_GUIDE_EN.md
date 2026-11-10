# **On My Disk** User Guide

Welcome to **On My Disk** — your private personal cloud platform. Unlike traditional cloud services, **On My Disk** keeps your files and data on your own devices, ensuring full control and privacy.

---

## 1. Getting Started (Onboarding)

You can start using **On My Disk** instantly by choosing one of the two access methods.

### Anonymous Login
If you are opening [**onmydisk**.net](https://disk.direct) for the first time, the system will offer a quick setup:
1.  **Device Name**: Enter a name for your current browser (e.g., "My Laptop").
2.  **Agreement**: Accept the Terms of Service (ToS) and GDPR Privacy Policy.
3.  **Connect a Folder**: You will be prompted to select a local folder for sharing. You can skip this step and add folders later.

> [!NOTE]
> Anonymous accounts are linked to a single device only and do not support multi-device access. However, since **On My Disk** works directly with your real file system, clearing your browser cache will not result in the loss of your files.

### Sign in with Google
The simplest way to access multiple devices is signing with Google account from each of them:
1.  Click on the profile icon in the bottom left corner.
2.  Select **Sign in with Google**.
3.  After authorization, you will see all your devices in 'My Devices' section.

### Create an **On My Disk** Account
If you prefer to use a separate account:
1.  Click on the profile icon and select **Sign up**.
2.  Enter your Email and create a password.
3.  An activation link will be sent to the specified address.
4.  Click the link in the email to confirm your registration.

> [!IMPORTANT]
> If the email doesn't arrive within a few minutes, be sure to check your **SPAM** folder. Registration confirmation emails sometimes end up there by mistake.

---

## 2. Connecting Folders

**On My Disk** allows you to turn any folder on your computer into part of your personal cloud.

You can add a folder in several ways:
1.  **During Onboarding**: The system will prompt you to select a folder during the first run.
2.  **In the Device root folder**: Navigate to your device (under the **My devices** or **All Files** section). If there are no folders yet, the **Add folder** button will be right in the center of the screen. If folders already exist, use the plus icon in the top toolbar.

After adding, the folder will appear in the **All files** -> **Device Name** section on all your connected devices.

Click **Add folder** to choose any directory on your device. Once added, you will gain secure remote access to it, allowing you to browse, manage, and share your files from anywhere.

---

## 3. Working with Files (File Manager)

The file manager interface is intuitive and functional.

### Main Sections (Sidebar):
*   **All files**: A general view of all your devices and connected folders.
*   **My devices**: A list of your physical devices.
*   **Shared to me / Shared by me**: Access to shared resources.
*   **Chats**: Access to the AI assistant.

### File Operations:
*   **View**: Click on a file to open it in the built-in viewer (supports PDF, images, video, text documents).
*   **Upload**: Use the **Upload** button or simply drag and drop files into the browser window.
*   **Search**: The search field in the top right corner helps you instantly find any document by name.

### Context Menu
Right-click on a file or folder to open the quick actions menu:
*   **Open/View**: Opens the file in the built-in viewer or editor.
*   **Download**: Saves the file to your current device. For multiple files, a **Download as ZIP** action is available.
*   **Rename**: Change the name of the file or folder.
*   **Copy/Move**: Choose a destination path to duplicate or transfer data.
*   **Delete**: Permanently remove the object from storage.
*   **Share**: Create a public link or configure access permissions (see the section below).
*   **Sign**: Ability to apply an electronic signature to PDF documents.

---

## 4. Creating Links and Sharing

**On My Disk** allows you to safely share files and folders with others.

### Public Links
To create a link to a file or folder:
1.  Right-click on the object and select **Share**.
2.  In the window that appears, you can:
    *   **Set a password**: Only people with the password will be able to see the content.
    *   **Expiration date**: The link will automatically stop working after a selected time (15 minutes, an hour, a day, etc.).
    *   **Allow changes**: If enabled, other users will be able to upload files to your folder or edit documents.
3.  Click **Create link**, and then copy the resulting address.

### Access Management
*   All links you create can be found in the **Shared by me** section.
*   There, you can change link settings at any time or completely revoke access using the **Revoke link** button.
*   Active shared resources are also displayed in the **Links** subfolder within your device.

---

## 5. Chat and AI Assistant (**OnMyChat**)

**OnMyChat** is your personal AI assistant (default — **June**) that uses your local data as its knowledge base.

### Key Features:
*   **Knowledge**: The assistant has access to your connected folders, as well as notes and imported links in the **Knowledge** section. You can ask about the content of your documents, and it will use them as context for its answers.
*   **Creations**: Ask the assistant to create an image or a short video based on your description. All generated files are saved directly to your storage.
*   **Privacy**: Chat history and the assistant's "memory" are stored only on your devices.

### Commands:
You can use the following slash commands to directly control the assistant's functions:

#### Working with Knowledge:
*   `/learn <path>` or `/import <path>`: Add a file or folder to your personal knowledge base. The assistant can then use the content of these documents to answer your questions.
*   `/recognize <path>`: Analyze an image (recognize text or objects) and add the description to the conversation context.

#### Image Generation:
*   `/generate <prompt>`: Create an image based on the text description. In "Creations" mode, the command is added automatically for consistency, but it can also be entered manually.
*   **Style Tags**: Add tags like `<Realistic>`, `<Fantasy>`, `<Perfect>`, `<Tooned>` to your prompt to specify the generation style.
*   **Character Tags**: Tags like `<Name>` allow using your saved characters (LoRA) when creating images.

> [!TIP]
> The list of available styles and characters is constantly updated. By installing your own **On My Chat** AI node, you can add any additional styles and characters from open libraries like [civitai.com](https://civitai.com).

## 6. Knowledge Base (Knowledge)

The **Knowledge** section allows you to manage the information that the AI assistant uses for its answers. This is your personal "brain," where indexed documents and your personal notes are stored.

### Section Features:
*   **View Sources**: This displays all files and folders that you have imported using the `/learn` or `/import` commands. You can see exactly which documents make up the assistant's knowledge base.
*   **Personal Notes**: You can create short text notes (Memos) directly in this section. This is convenient for saving facts that are not contained in separate files.
*   **Hashtags and Relevance**: Use hashtags (e.g., `#project`, `#important`, `#instruction`) inside your notes and descriptions. This significantly increases the probability that the assistant will choose these specific facts when searching for an answer to your question if it contains similar keywords.
*   **Data Management**: Any imported source or note can be deleted or edited, thus updating the assistant's "memory."

Using the Knowledge Base makes the assistant's answers more accurate and tailored to your specific context.

---

## 7. Settings

In the settings section, you can fully personalize the system.

### Settings Tabs:
*   **Profile**: Information about your plan (FREE/Pro), account ID, and API token.
*   **Device**: Manage device name and the list of connected folders.
*   **Assistant**: Configure your AI assistant:
    *   Change the name and personality.
    *   Select a folder for saving generated materials.
*   **Viewer**: Set preferences for viewing documents and media files.

---

**On My Disk** is freedom from corporate clouds and security for your data. Enjoy your privacy!
