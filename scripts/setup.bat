mkdir C:\chromium\src\out\thorium

copy ..\build\* C:\chromium\src\build
copy "..\chrome\*" "C:\chromium\src\chrome"
copy ..\components\* C:\chromium\src\components
copy ..\content\* C:\chromium\src\content
copy ..\media\* C:\chromium\src\media
copy ..\net\* C:\chromium\src\net
copy ..\ui\* C:\chromium\src\ui
copy ..\v8\* C:\chromium\src\v8
copy ..\chrome-devtools\chrome-devtools.svg C:\chromium\src\out\thorium

cd C:\chromium\src

exit 0
